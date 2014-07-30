#include "mednafen/mednafen.h"
#include "mednafen/mempatcher.h"
#include "mednafen/git.h"
#include "mednafen/general.h"
#include "mednafen/md5.h"
#ifdef NEED_DEINTERLACER
#include	"mednafen/video/Deinterlacer.h"
#endif
#include "libretro.h"
#include "thread.h"

static MDFNGI *game;

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;
retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static bool overscan;
static double last_sound_rate;
static MDFN_PixelFormat last_pixel_format;

static MDFN_Surface *surf;

static bool failed_init;

static void hookup_ports(bool force);

static bool initial_ports_hookup = false;

std::string retro_base_directory;
std::string retro_base_name;
std::string retro_save_directory;

/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "mednafen/pcfx/pcfx.h"
#include "mednafen/pcfx/soundbox.h"
#include "mednafen/pcfx/input.h"
#include "mednafen/pcfx/king.h"
#include "mednafen/pcfx/timer.h"
#include "mednafen/pcfx/interrupt.h"
#include "mednafen/pcfx/debug.h"
#include "mednafen/pcfx/rainbow.h"
#include "mednafen/pcfx/huc6273.h"
#include "mednafen/pcfx/fxscsi.h"
#include "mednafen/cdrom/scsicd.h"
#include "mednafen/mempatcher.h"
#include "mednafen/cdrom/cdromif.h"
#include "mednafen/md5.h"
#include "mednafen/clamp.h"

#include <trio/trio.h>
#include <errno.h>
#include <string.h>
#include <math.h>

/* FIXME:  soundbox, vce, vdc, rainbow, and king store wait states should be 4, not 2, but V810 has write buffers which can mask wait state penalties.
  This is a hack to somewhat address the issue, but to really fix it, we need to handle write buffer emulation in the V810 emulation core itself.
*/

static std::vector<CDIF*> *cdifs = NULL;
static bool CD_TrayOpen;
static int CD_SelectedDisc;	// -1 for no disc

V810 PCFX_V810;

static uint8 *BIOSROM = NULL; 	// 1MB
static uint8 *RAM = NULL; 	// 2MB
static uint8 *FXSCSIROM = NULL;	// 512KiB

static uint32 RAM_LPA;		// Last page access

static const int RAM_PageSize = 2048;
static const int RAM_PageNOTMask = ~(RAM_PageSize - 1);

static uint16 Last_VDC_AR[2];

static bool WantHuC6273 = FALSE;

//static 
VDC *fx_vdc_chips[2];

static uint16 BackupControl;
static uint8 BackupRAM[0x8000], ExBackupRAM[0x8000];
static uint8 ExBusReset; // I/O Register at 0x0700

static bool BRAMDisabled;	// Cached at game load, don't remove this caching behavior or save game loss may result(if we ever get a GUI).

// Checks to see if this main-RAM-area access
// is in the same DRAM page as the last access.
#define RAMLPCHECK	\
{					\
  if((A & RAM_PageNOTMask) != RAM_LPA)	\
  {					\
   timestamp += 3;			\
   RAM_LPA = A & RAM_PageNOTMask;	\
  }					\
}

static v810_timestamp_t next_pad_ts, next_timer_ts, next_adpcm_ts, next_king_ts;

void PCFX_FixNonEvents(void)
{
 if(next_pad_ts & 0x40000000)
  next_pad_ts = PCFX_EVENT_NONONO;

 if(next_timer_ts & 0x40000000)
  next_timer_ts = PCFX_EVENT_NONONO;

 if(next_adpcm_ts & 0x40000000)
  next_adpcm_ts = PCFX_EVENT_NONONO;

 if(next_king_ts & 0x40000000)
  next_king_ts = PCFX_EVENT_NONONO;
}

void PCFX_Event_Reset(void)
{
 next_pad_ts = PCFX_EVENT_NONONO;
 next_timer_ts = PCFX_EVENT_NONONO;
 next_adpcm_ts = PCFX_EVENT_NONONO;
 next_king_ts = PCFX_EVENT_NONONO;
}

static INLINE uint32 CalcNextTS(void)
{
 v810_timestamp_t next_timestamp = next_king_ts;

 if(next_timestamp > next_pad_ts)
  next_timestamp  = next_pad_ts;

 if(next_timestamp > next_timer_ts)
  next_timestamp = next_timer_ts;

 if(next_timestamp > next_adpcm_ts)
  next_timestamp = next_adpcm_ts;

 return(next_timestamp);
}

static void RebaseTS(const v810_timestamp_t timestamp, const v810_timestamp_t new_base_timestamp)
{
 assert(next_pad_ts > timestamp);
 assert(next_timer_ts > timestamp);
 assert(next_adpcm_ts > timestamp);
 assert(next_king_ts > timestamp);

 next_pad_ts -= (timestamp - new_base_timestamp);
 next_timer_ts -= (timestamp - new_base_timestamp);
 next_adpcm_ts -= (timestamp - new_base_timestamp);
 next_king_ts -= (timestamp - new_base_timestamp);

 //printf("RTS: %d %d %d %d\n", next_pad_ts, next_timer_ts, next_adpcm_ts, next_king_ts);
}


void PCFX_SetEvent(const int type, const v810_timestamp_t next_timestamp)
{
 //assert(next_timestamp > PCFX_V810.v810_timestamp);

 if(type == PCFX_EVENT_PAD)
  next_pad_ts = next_timestamp;
 else if(type == PCFX_EVENT_TIMER)
  next_timer_ts = next_timestamp;
 else if(type == PCFX_EVENT_ADPCM)
  next_adpcm_ts = next_timestamp;
 else if(type == PCFX_EVENT_KING)
  next_king_ts = next_timestamp;

 if(next_timestamp < PCFX_V810.GetEventNT())
  PCFX_V810.SetEventNT(next_timestamp);
}

int32 MDFN_FASTCALL pcfx_event_handler(const v810_timestamp_t timestamp)
{
     if(timestamp >= next_king_ts)
      next_king_ts = KING_Update(timestamp);

     if(timestamp >= next_pad_ts)
      next_pad_ts = FXINPUT_Update(timestamp);

     if(timestamp >= next_timer_ts)
      next_timer_ts = FXTIMER_Update(timestamp);

     if(timestamp >= next_adpcm_ts)
      next_adpcm_ts = SoundBox_ADPCMUpdate(timestamp);

#if 1
     assert(next_king_ts > timestamp);
     assert(next_pad_ts > timestamp);
     assert(next_timer_ts > timestamp);
     assert(next_adpcm_ts > timestamp);
#endif
     return(CalcNextTS());
}

// Called externally from debug.cpp
void ForceEventUpdates(const uint32 timestamp)
{
 next_king_ts = KING_Update(timestamp);
 next_pad_ts = FXINPUT_Update(timestamp);
 next_timer_ts = FXTIMER_Update(timestamp);
 next_adpcm_ts = SoundBox_ADPCMUpdate(timestamp);

 //printf("Meow: %d\n", CalcNextTS());
 PCFX_V810.SetEventNT(CalcNextTS());

 //printf("FEU: %d %d %d %d\n", next_pad_ts, next_timer_ts, next_adpcm_ts, next_king_ts);
}

#include "io-handler.inc"
#include "mem-handler.inc"

typedef struct
{
 int8 tracknum;
 int8 format;
 uint32 lba;
} CDGameEntryTrack;

typedef struct
{
 const char *name;
 const char *name_original;     // Original non-Romanized text.
 const uint32 flags;            // Emulation flags.
 const unsigned int discs;      // Number of discs for this game.
 CDGameEntryTrack tracks[2][100]; // 99 tracks and 1 leadout track
} CDGameEntry;

#define CDGE_FORMAT_AUDIO		0
#define CDGE_FORMAT_DATA		1

#define CDGE_FLAG_ACCURATE_V810         0x01
#define CDGE_FLAG_FXGA			0x02

static uint32 EmuFlags;

static CDGameEntry GameList[] =
{
 #include "gamedb.inc"
};


static void Emulate(EmulateSpecStruct *espec)
{
 v810_timestamp_t v810_timestamp;
 v810_timestamp_t new_base_ts;

 //printf("%d\n", PCFX_V810.v810_timestamp);

 FXINPUT_Frame();

 MDFNMP_ApplyPeriodicCheats();

 if(espec->VideoFormatChanged)
  KING_SetPixelFormat(espec->surface->format); //.Rshift, espec->surface->format.Gshift, espec->surface->format.Bshift);

 if(espec->SoundFormatChanged)
  SoundBox_SetSoundRate(espec->SoundRate);


 KING_StartFrame(fx_vdc_chips, espec);	//espec->surface, &espec->DisplayRect, espec->LineWidths, espec->skip);

 v810_timestamp = PCFX_V810.Run(pcfx_event_handler);


 PCFX_FixNonEvents();

 // Call before resetting v810_timestamp
 ForceEventUpdates(v810_timestamp);

 //
 // new_base_ts is guaranteed to be <= v810_timestamp
 //
 espec->SoundBufSize = SoundBox_Flush(v810_timestamp, &new_base_ts, espec->SoundBuf, espec->SoundBufMaxSize);

 KING_EndFrame(v810_timestamp, new_base_ts);
 FXTIMER_ResetTS(new_base_ts);
 FXINPUT_ResetTS(new_base_ts);
 SoundBox_ResetTS(new_base_ts);

 // Call this AFTER all the EndFrame/Flush/ResetTS stuff
 RebaseTS(v810_timestamp, new_base_ts);

 espec->MasterCycles = v810_timestamp - new_base_ts;

 PCFX_V810.ResetTS(new_base_ts);
}

static void PCFX_Reset(void)
{
 const uint32 timestamp = PCFX_V810.v810_timestamp;

 //printf("Reset: %d\n", timestamp);

 // Make sure all devices are synched to current timestamp before calling their Reset()/Power()(though devices should already do this sort of thing on their
 // own, but it's not implemented for all of them yet, and even if it was all implemented this is also INSURANCE).
 ForceEventUpdates(timestamp);

 PCFX_Event_Reset();

 RAM_LPA = 0;

 ExBusReset = 0;
 BackupControl = 0;

 Last_VDC_AR[0] = 0;
 Last_VDC_AR[1] = 0;

 memset(RAM, 0x00, 2048 * 1024);

 for(int i = 0; i < 2; i++)
 {
  int32 dummy_ne MDFN_NOWARN_UNUSED;

  dummy_ne = fx_vdc_chips[i]->Reset();
 }

 KING_Reset(timestamp);	// SCSICD_Power() is called from KING_Reset()
 SoundBox_Reset(timestamp);
 RAINBOW_Reset();

 if(WantHuC6273)
  HuC6273_Reset();

 PCFXIRQ_Reset();
 FXTIMER_Reset();
 PCFX_V810.Reset();

 // Force device updates so we can get new next event timestamp values.
 ForceEventUpdates(timestamp);
}

static void PCFX_Power(void)
{
 PCFX_Reset();
}

static void VDCA_IRQHook(bool asserted)
{
 PCFXIRQ_Assert(PCFXIRQ_SOURCE_VDCA, asserted);
}

static void VDCB_IRQHook(bool asserted)
{
 PCFXIRQ_Assert(PCFXIRQ_SOURCE_VDCB, asserted);
}

static void SetRegGroups(void);

static bool LoadCommon(std::vector<CDIF *> *CDInterfaces)
{
 std::string biospath = MDFN_MakeFName(MDFNMKF_FIRMWARE, 0, MDFN_GetSettingS("pcfx.bios").c_str());
 std::string fxscsi_path = MDFN_GetSettingS("pcfx.fxscsi");	// For developers only, so don't make it convenient.
 MDFNFILE BIOSFile;
 V810_Emu_Mode cpu_mode;


 if(!BIOSFile.Open(biospath, NULL, "BIOS"))
  return(0);

 cpu_mode = (V810_Emu_Mode)MDFN_GetSettingI("pcfx.cpu_emulation");
 if(cpu_mode == _V810_EMU_MODE_COUNT)
 {
  cpu_mode = (EmuFlags & CDGE_FLAG_ACCURATE_V810) ? V810_EMU_MODE_ACCURATE : V810_EMU_MODE_FAST;
 }

 if(EmuFlags & CDGE_FLAG_FXGA)
 {
  //WantHuC6273 = TRUE;
 }

 MDFN_printf(_("V810 Emulation Mode: %s\n"), (cpu_mode == V810_EMU_MODE_ACCURATE) ? _("Accurate") : _("Fast"));
 PCFX_V810.Init(cpu_mode, false);

 uint32 RAM_Map_Addresses[1] = { 0x00000000 };
 uint32 BIOSROM_Map_Addresses[1] = { 0xFFF00000 };

 // todo: cleanup on error
 if(!(RAM = PCFX_V810.SetFastMap(RAM_Map_Addresses, 0x00200000, 1, _("RAM"))))
 {
  return(0);
 }

 if(!(BIOSROM = PCFX_V810.SetFastMap(BIOSROM_Map_Addresses, 0x00100000, 1, _("BIOS ROM"))))
 {
  return(0);
 }

 if(GET_FSIZE(BIOSFile) != 1024 * 1024)
 {
  MDFN_PrintError(_("BIOS ROM file is incorrect size.\n"));
  return(0);
 }

 memcpy(BIOSROM, GET_FDATA(BIOSFile), 1024 * 1024);

 BIOSFile.Close();

#if 0
 if(fxscsi_path != "0" && fxscsi_path != "" && fxscsi_path != "none")
 {
  MDFNFILE FXSCSIFile;

  if(!FXSCSIFile.Open(fxscsi_path, NULL, "FX-SCSI ROM"))
   return(0);

  if(GET_FSIZE(FXSCSIFile) != 1024 * 512)
  {
   MDFN_PrintError(_("BIOS ROM file is incorrect size.\n"));
   return(0);
  }

  uint32 FXSCSI_Map_Addresses[1] = { 0x80780000 };

  if(!(FXSCSIROM = PCFX_V810.SetFastMap(FXSCSI_Map_Addresses, 0x0080000, 1, _("FX-SCSI ROM"))))
  {
   return(0);
  }

  memcpy(FXSCSIROM, GET_FDATA(FXSCSIFile), 1024 * 512);

  FXSCSIFile.Close();
 }
#endif

 for(int i = 0; i < 2; i++)
 {
  fx_vdc_chips[i] = new VDC(MDFN_GetSettingB("pcfx.nospritelimit"), 65536);
  fx_vdc_chips[i]->SetWSHook(NULL);
  fx_vdc_chips[i]->SetIRQHook(i ? VDCB_IRQHook : VDCA_IRQHook);

  //fx_vdc_chips[0] = FXVDC_Init(PCFXIRQ_SOURCE_VDCA, MDFN_GetSettingB("pcfx.nospritelimit"));
  //fx_vdc_chips[1] = FXVDC_Init(PCFXIRQ_SOURCE_VDCB, MDFN_GetSettingB("pcfx.nospritelimit"));
 }

 SoundBox_Init(MDFN_GetSettingB("pcfx.adpcm.emulate_buggy_codec"), MDFN_GetSettingB("pcfx.adpcm.suppress_channel_reset_clicks"));
 RAINBOW_Init(MDFN_GetSettingB("pcfx.rainbow.chromaip"));
 FXINPUT_Init();
 FXTIMER_Init();

 if(WantHuC6273)
  HuC6273_Init();

 if(!KING_Init())
 {
  MDFN_free(BIOSROM);
  MDFN_free(RAM);
  BIOSROM = NULL;
  RAM = NULL;
  return(0);
 }

 CD_TrayOpen = false;
 CD_SelectedDisc = 0;

 SCSICD_SetDisc(true, NULL, true);
 SCSICD_SetDisc(false, (*CDInterfaces)[0], true);




 MDFNGameInfo->fps = (uint32)((double)7159090.90909090 / 455 / 263 * 65536 * 256);

 MDFNGameInfo->nominal_height = MDFN_GetSettingUI("pcfx.slend") - MDFN_GetSettingUI("pcfx.slstart") + 1;

 // Emulation raw framebuffer image should always be of 256 width when the pcfx.high_dotclock_width setting is set to "256",
 // but it could be either 256 or 341 when the setting is set to "341", so stay with 1024 in that case so we won't have
 // a messed up aspect ratio in our recorded QuickTime movies.
 MDFNGameInfo->lcm_width = (MDFN_GetSettingUI("pcfx.high_dotclock_width") == 256) ? 256 : 1024;
 MDFNGameInfo->lcm_height = MDFNGameInfo->nominal_height;

 MDFNMP_Init(1024 * 1024, ((uint64)1 << 32) / (1024 * 1024));
 MDFNMP_AddRAM(2048 * 1024, 0x00000000, RAM);


 BRAMDisabled = MDFN_GetSettingB("pcfx.disable_bram");

 if(BRAMDisabled)
  MDFN_printf(_("Warning: BRAM is disabled per pcfx.disable_bram setting.  This is simulating a malfunction.\n"));

 if(!BRAMDisabled)
 {
  // Initialize backup RAM
  memset(BackupRAM, 0, sizeof(BackupRAM));
  memset(ExBackupRAM, 0, sizeof(ExBackupRAM));

  static const uint8 BRInit00[] = { 0x24, 0x8A, 0xDF, 0x50, 0x43, 0x46, 0x58, 0x53, 0x72, 0x61, 0x6D, 0x80,
                                   0x00, 0x01, 0x01, 0x00, 0x01, 0x40, 0x00, 0x00, 0x01, 0xF9, 0x03, 0x00,
                                   0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
                                  };
  static const uint8 BRInit80[] = { 0xF9, 0xFF, 0xFF };

  memcpy(BackupRAM + 0x00, BRInit00, sizeof(BRInit00));
  memcpy(BackupRAM + 0x80, BRInit80, sizeof(BRInit80));


  static const uint8 ExBRInit00[] = { 0x24, 0x8A, 0xDF, 0x50, 0x43, 0x46, 0x58, 0x43, 0x61, 0x72, 0x64, 0x80,
                                     0x00, 0x01, 0x01, 0x00, 0x01, 0x40, 0x00, 0x00, 0x01, 0xF9, 0x03, 0x00,
                                     0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
                                  };
  static const uint8 ExBRInit80[] = { 0xF9, 0xFF, 0xFF };

  memcpy(ExBackupRAM + 0x00, ExBRInit00, sizeof(ExBRInit00));
  memcpy(ExBackupRAM + 0x80, ExBRInit80, sizeof(ExBRInit80));

  FILE *savefp;
  if((savefp = gzopen(MDFN_MakeFName(MDFNMKF_SAV, 0, "sav").c_str(), "rb")))
  {
   gzread(savefp, BackupRAM, 0x8000);
   gzread(savefp, ExBackupRAM, 0x8000);
   gzclose(savefp);
  }
 }

 // Default to 16-bit bus.
 for(int i = 0; i < 256; i++)
 {
  PCFX_V810.SetMemReadBus32(i, FALSE);
  PCFX_V810.SetMemWriteBus32(i, FALSE);
 }

 // 16MiB RAM area.
 PCFX_V810.SetMemReadBus32(0, TRUE);
 PCFX_V810.SetMemWriteBus32(0, TRUE);

 // Bitstring read range
 for(int i = 0xA0; i <= 0xAF; i++)
 {
  PCFX_V810.SetMemReadBus32(i, FALSE);       // Reads to the read range are 16-bit, and
  PCFX_V810.SetMemWriteBus32(i, TRUE);       // writes are 32-bit.
 }

 // Bitstring write range
 for(int i = 0xB0; i <= 0xBF; i++)
 {
  PCFX_V810.SetMemReadBus32(i, TRUE);	// Reads to the write range are 32-bit,
  PCFX_V810.SetMemWriteBus32(i, FALSE);	// but writes are 16-bit!
 }

 // BIOS area
 for(int i = 0xF0; i <= 0xFF; i++)
 {
  PCFX_V810.SetMemReadBus32(i, FALSE);
  PCFX_V810.SetMemWriteBus32(i, FALSE);
 }

 PCFX_V810.SetMemReadHandlers(mem_rbyte, mem_rhword, mem_rword);
 PCFX_V810.SetMemWriteHandlers(mem_wbyte, mem_whword, mem_wword);

 PCFX_V810.SetIOReadHandlers(port_rbyte, port_rhword, NULL);
 PCFX_V810.SetIOWriteHandlers(port_wbyte, port_whword, NULL);



 return(1);
}

static void DoMD5CDVoodoo(std::vector<CDIF *> *CDInterfaces)
{
 const CDGameEntry *found_entry = NULL;
 CDUtility::TOC toc;

 #if 0
 puts("{");
 puts(" ,");
 puts(" ,");
 puts(" 0,");
 puts(" 1,");
 puts(" {");
 puts("  {");

 for(int i = CDIF_GetFirstTrack(); i <= CDIF_GetLastTrack(); i++)
 {
  CDIF_Track_Format tf;

  CDIF_GetTrackFormat(i, tf);
  
  printf("   { %d, %s, %d },\n", i, (tf == CDIF_FORMAT_AUDIO) ? "CDIF_FORMAT_AUDIO" : "CDIF_FORMAT_MODE1", CDIF_GetTrackStartPositionLBA(i));
 }
 printf("   { -1, (CDIF_Track_Format)-1, %d },\n", CDIF_GetSectorCountLBA());
 puts("  }");
 puts(" }");
 puts("},");
 //exit(1);
 #endif

 for(unsigned if_disc = 0; if_disc < CDInterfaces->size(); if_disc++)
 {
  (*CDInterfaces)[if_disc]->ReadTOC(&toc);

  if(toc.first_track == 1)
  {
   for(unsigned int g = 0; g < sizeof(GameList) / sizeof(CDGameEntry); g++)
   {
    const CDGameEntry *entry = &GameList[g];

    assert(entry->discs == 1 || entry->discs == 2);

    for(unsigned int disc = 0; disc < entry->discs; disc++)
    {
     const CDGameEntryTrack *et = entry->tracks[disc];
     bool GameFound = TRUE;

     while(et->tracknum != -1 && GameFound)
     {
      assert(et->tracknum > 0 && et->tracknum < 100);

      if(toc.tracks[et->tracknum].lba != et->lba)
       GameFound = FALSE;

      if( ((et->format == CDGE_FORMAT_DATA) ? 0x4 : 0x0) != (toc.tracks[et->tracknum].control & 0x4))
       GameFound = FALSE;

      et++;
     }

     if(et->tracknum == -1)
     {
      if((et - 1)->tracknum != toc.last_track)
       GameFound = FALSE;
 
      if(et->lba != toc.tracks[100].lba)
       GameFound = FALSE;
     }

     if(GameFound)
     {
      found_entry = entry;
      goto FoundIt;
     }
    } // End disc count loop
   }
  }

  FoundIt: ;

  if(found_entry)
  {
   EmuFlags = found_entry->flags;

   if(found_entry->discs > 1)
   {
    const char *hash_prefix = "Mednafen PC-FX Multi-Game Set";
    md5_context md5_gameset;

    md5_gameset.starts();

    md5_gameset.update_string(hash_prefix);

    for(unsigned int disc = 0; disc < found_entry->discs; disc++)
    {
     const CDGameEntryTrack *et = found_entry->tracks[disc];

     while(et->tracknum)
     {
      md5_gameset.update_u32_as_lsb(et->tracknum);
      md5_gameset.update_u32_as_lsb((uint32)et->format);
      md5_gameset.update_u32_as_lsb(et->lba);

      if(et->tracknum == -1)
       break;
      et++;
     }
    }
    md5_gameset.finish(MDFNGameInfo->GameSetMD5);
    MDFNGameInfo->GameSetMD5Valid = TRUE;
   }
   //printf("%s\n", found_entry->name);
   MDFNGameInfo->name = (UTF8*)strdup(found_entry->name);
   break;
  }
 } // end: for(unsigned if_disc = 0; if_disc < CDInterfaces->size(); if_disc++)

 MDFN_printf(_("CD Layout MD5:   0x%s\n"), md5_context::asciistr(MDFNGameInfo->MD5, 0).c_str());

 if(MDFNGameInfo->GameSetMD5Valid)
  MDFN_printf(_("GameSet MD5:     0x%s\n"), md5_context::asciistr(MDFNGameInfo->GameSetMD5, 0).c_str());
}

// PC-FX BIOS will look at all data tracks(not just the first one), in contrast to the PCE CD BIOS, which only looks
// at the first data track.
static bool TestMagicCD(std::vector<CDIF *> *CDInterfaces)
{
 CDIF *cdiface = (*CDInterfaces)[0];
 CDUtility::TOC toc;
 uint8 sector_buffer[2048];

 memset(sector_buffer, 0, sizeof(sector_buffer));

 cdiface->ReadTOC(&toc);

 for(int32 track = toc.first_track; track <= toc.last_track; track++)
 {
  if(toc.tracks[track].control & 0x4)
  {
   cdiface->ReadSector(sector_buffer, toc.tracks[track].lba, 1);
   if(!strncmp("PC-FX:Hu_CD-ROM", (char*)sector_buffer, strlen("PC-FX:Hu_CD-ROM")))
   {
    return(TRUE);
   }

   if(!strncmp((char *)sector_buffer + 64, "PPPPHHHHOOOOTTTTOOOO____CCCCDDDD", 32))
    return(true);
  }
 }
 return(FALSE);
}

static int LoadCD(std::vector<CDIF *> *CDInterfaces)
{
 EmuFlags = 0;

 cdifs = CDInterfaces;

 DoMD5CDVoodoo(CDInterfaces);

 if(!LoadCommon(CDInterfaces))
  return(0);

 MDFN_printf(_("Emulated CD-ROM drive speed: %ux\n"), (unsigned int)MDFN_GetSettingUI("pcfx.cdspeed"));

 MDFNGameInfo->GameType = GMT_CDROM;

 PCFX_Power();

 return(1);
}

static void PCFX_CDInsertEject(void)
{
 CD_TrayOpen = !CD_TrayOpen;

 for(unsigned disc = 0; disc < cdifs->size(); disc++)
 {
  if(!(*cdifs)[disc]->Eject(CD_TrayOpen))
  {
   MDFN_DispMessage(_("Eject error."));
   CD_TrayOpen = !CD_TrayOpen;
  }
 }

 if(CD_TrayOpen)
  MDFN_DispMessage(_("Virtual CD Drive Tray Open"));
 else
  MDFN_DispMessage(_("Virtual CD Drive Tray Closed"));

 SCSICD_SetDisc(CD_TrayOpen, (CD_SelectedDisc >= 0 && !CD_TrayOpen) ? (*cdifs)[CD_SelectedDisc] : NULL);
}

static void PCFX_CDEject(void)
{
 if(!CD_TrayOpen)
  PCFX_CDInsertEject();
}

static void PCFX_CDSelect(void)
{
 if(cdifs && CD_TrayOpen)
 {
  CD_SelectedDisc = (CD_SelectedDisc + 1) % (cdifs->size() + 1);

  if((unsigned)CD_SelectedDisc == cdifs->size())
   CD_SelectedDisc = -1;

  if(CD_SelectedDisc == -1)
   MDFN_DispMessage(_("Disc absence selected."));
  else
   MDFN_DispMessage(_("Disc %d of %d selected."), CD_SelectedDisc + 1, (int)cdifs->size());
 }
}

static void CloseGame(void)
{
 if(!BRAMDisabled)
 {
  std::vector<PtrLengthPair> EvilRams;
 
  EvilRams.push_back(PtrLengthPair(BackupRAM, 0x8000));
  EvilRams.push_back(PtrLengthPair(ExBackupRAM, 0x8000));

  MDFN_DumpToFile(MDFN_MakeFName(MDFNMKF_SAV, 0, "sav").c_str(), 0, EvilRams);
 }

 for(int i = 0; i < 2; i++)
  if(fx_vdc_chips[i])
  {
   delete fx_vdc_chips[i];
   fx_vdc_chips[i] = NULL;
  }

 RAINBOW_Close();
 KING_Close();
 PCFX_V810.Kill();

 // The allocated memory RAM and BIOSROM is free'd in V810_Kill()
 RAM = NULL;
 BIOSROM = NULL;
}

static void DoSimpleCommand(int cmd)
{
 switch(cmd)
 {
   case MDFN_MSC_INSERT_DISK:
		PCFX_CDInsertEject();
                break;

   case MDFN_MSC_SELECT_DISK:
		PCFX_CDSelect();
                break;

   case MDFN_MSC_EJECT_DISK:
		PCFX_CDEject();
                break;

  case MDFN_MSC_RESET: PCFX_Reset(); break;
  case MDFN_MSC_POWER: PCFX_Power(); break;
 }
}

static int StateAction(StateMem *sm, int load, int data_only)
{
 const v810_timestamp_t timestamp = PCFX_V810.v810_timestamp;

 SFORMAT StateRegs[] =
 {
  SFARRAY(RAM, 0x200000),
  SFARRAY16(Last_VDC_AR, 2),
  SFVAR(BackupControl),
  SFVAR(ExBusReset),
  SFARRAY(BackupRAM, BRAMDisabled ? 0 : 0x8000),
  SFARRAY(ExBackupRAM, BRAMDisabled ? 0 : 0x8000),

  SFVAR(CD_TrayOpen),
  SFVAR(CD_SelectedDisc),

  SFEND
 };

 int ret = MDFNSS_StateAction(sm, load, data_only, StateRegs, "MAIN");

 for(int i = 0; i < 2; i++)
  ret &= fx_vdc_chips[i]->StateAction(sm, load, data_only, i ? "VDC1" : "VDC0");

 ret &= FXINPUT_StateAction(sm, load, data_only);
 ret &= PCFXIRQ_StateAction(sm, load, data_only);
 ret &= KING_StateAction(sm, load, data_only);
 ret &= PCFX_V810.StateAction(sm, load, data_only);
 ret &= FXTIMER_StateAction(sm, load, data_only);
 ret &= SoundBox_StateAction(sm, load, data_only);
 ret &= SCSICD_StateAction(sm, load, data_only, "CDRM");
 ret &= RAINBOW_StateAction(sm, load, data_only);

 if(load)
 {
  //
  // Rather than bothering to store next event timestamp deltas in save states, we'll just recalculate next event times on save state load as a side effect
  // of this call.
  //
  ForceEventUpdates(timestamp);

  if(cdifs)
  {
   // Sanity check.
   if(CD_SelectedDisc >= (int)cdifs->size())
    CD_SelectedDisc = (int)cdifs->size() - 1;

   SCSICD_SetDisc(CD_TrayOpen, (CD_SelectedDisc >= 0 && !CD_TrayOpen) ? (*cdifs)[CD_SelectedDisc] : NULL, true);
  }
 }

 //printf("0x%08x, %d %d %d %d\n", load, next_pad_ts, next_timer_ts, next_adpcm_ts, next_king_ts);

 return(ret);
}

static const MDFNSetting_EnumList V810Mode_List[] =
{
 { "fast", (int)V810_EMU_MODE_FAST, gettext_noop("Fast Mode"), gettext_noop("Fast mode trades timing accuracy, cache emulation, and executing from hardware registers and RAM not intended for code use for performance.")},
 { "accurate", (int)V810_EMU_MODE_ACCURATE, gettext_noop("Accurate Mode"), gettext_noop("Increased timing accuracy, though not perfect, along with cache emulation, at the cost of decreased performance.  Additionally, even the pipeline isn't correctly and fully emulated in this mode.") },
 { "auto", (int)_V810_EMU_MODE_COUNT, gettext_noop("Auto Mode"), gettext_noop("Selects \"fast\" or \"accurate\" automatically based on an internal database.  If the CD image is not recognized, defaults to \"fast\".") },
 { NULL, 0 },
};


static const MDFNSetting_EnumList HDCWidthList[] =
{
 { "256", 256,	"256 pixels", gettext_noop("This value will cause heavy pixel distortion.") },
 { "341", 341,	"341 pixels", gettext_noop("This value will cause moderate pixel distortion.") },
 { "1024", 1024, "1024 pixels", gettext_noop("This value will cause no pixel distortion as long as interpolation is enabled on the video output device and the resolution is sufficiently high, but it will use a lot of CPU time.") },
 { NULL, 0 },
};

static MDFNSetting PCFXSettings[] =
{
  { "pcfx.input.port1.multitap", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, gettext_noop("Enable multitap on PC-FX port 1."), gettext_noop("EXPERIMENTAL emulation of the unreleased multitap.  Enables ports 3 4 5."), MDFNST_BOOL, "0", NULL, NULL },
  { "pcfx.input.port2.multitap", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, gettext_noop("Enable multitap on PC-FX port 2."), gettext_noop("EXPERIMENTAL emulation of the unreleased multitap.  Enables ports 6 7 8."), MDFNST_BOOL, "0", NULL, NULL },


  { "pcfx.mouse_sensitivity", MDFNSF_NOFLAGS, gettext_noop("Mouse sensitivity."), NULL, MDFNST_FLOAT, "1.25", NULL, NULL },
  { "pcfx.disable_softreset", MDFNSF_NOFLAGS, gettext_noop("When RUN+SEL are pressed simultaneously, disable both buttons temporarily."), NULL, MDFNST_BOOL, "0", NULL, NULL, NULL, FXINPUT_SettingChanged },

  { "pcfx.cpu_emulation", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, gettext_noop("CPU emulation mode."), NULL, MDFNST_ENUM, "auto", NULL, NULL, NULL, NULL, V810Mode_List },
  { "pcfx.bios", MDFNSF_EMU_STATE, gettext_noop("Path to the ROM BIOS"), NULL, MDFNST_STRING, "pcfx.rom" },
  { "pcfx.fxscsi", MDFNSF_EMU_STATE, gettext_noop("Path to the FX-SCSI ROM"), gettext_noop("Intended for developers only."), MDFNST_STRING, "0" },
  { "pcfx.disable_bram", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, gettext_noop("Disable internal and external BRAM."), gettext_noop("It is intended for viewing games' error screens that may be different from simple BRAM full and uninitialized BRAM error screens, though it can cause the game to crash outright."), MDFNST_BOOL, "0" },
  { "pcfx.cdspeed", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, gettext_noop("Emulated CD-ROM speed."), gettext_noop("Setting the value higher than 2, the default, will decrease loading times in most games by some degree."), MDFNST_UINT, "2", "2", "10" },

  { "pcfx.nospritelimit", MDFNSF_NOFLAGS, gettext_noop("Remove 16-sprites-per-scanline hardware limit."), NULL, MDFNST_BOOL, "0" },
  { "pcfx.high_dotclock_width", MDFNSF_NOFLAGS, gettext_noop("Emulated width for 7.16MHz dot-clock mode."), gettext_noop("Lower values are faster, but will cause some degree of pixel distortion."), MDFNST_ENUM, "1024", NULL, NULL, NULL, NULL, HDCWidthList },

  { "pcfx.slstart", MDFNSF_NOFLAGS, gettext_noop("First rendered scanline."), NULL, MDFNST_UINT, "4", "0", "239" },
  { "pcfx.slend", MDFNSF_NOFLAGS, gettext_noop("Last rendered scanline."), NULL, MDFNST_UINT, "235", "0", "239" },

  { "pcfx.rainbow.chromaip", MDFNSF_NOFLAGS, gettext_noop("Enable bilinear interpolation on the chroma channel of RAINBOW YUV output."), gettext_noop("This is an enhancement-related setting.  Enabling it may cause graphical glitches with some games."), MDFNST_BOOL, "0" },

  { "pcfx.adpcm.suppress_channel_reset_clicks", MDFNSF_NOFLAGS, gettext_noop("Hack to suppress clicks caused by forced channel resets."), NULL, MDFNST_BOOL, "1" },

  // Hack that emulates the codec a buggy ADPCM encoder used for some games' ADPCM.  Not enabled by default because it makes some games(with 
  //correctly-encoded  ADPCM?) sound worse
  { "pcfx.adpcm.emulate_buggy_codec", MDFNSF_NOFLAGS, gettext_noop("Hack that emulates the codec a buggy ADPCM encoder used for some games' ADPCM."), NULL, MDFNST_BOOL, "0" },

  { "pcfx.resamp_quality", MDFNSF_NOFLAGS, gettext_noop("Sound quality."), gettext_noop("Higher values correspond to better SNR and better preservation of higher frequencies(\"brightness\"), at the cost of increased computational complexity and a negligible increase in latency."), MDFNST_INT, "3", "0", "5" },
  { "pcfx.resamp_rate_error", MDFNSF_NOFLAGS, gettext_noop("Output rate tolerance."), gettext_noop("Lower values correspond to better matching of the output rate of the resampler to the actual desired output rate, at the expense of increased RAM usage and poorer CPU cache utilization."), MDFNST_FLOAT, "0.0000009", "0.0000001", "0.0000350" },

  { NULL }
};

static const FileExtensionSpecStruct KnownExtensions[] =
{
 //{ ".ex", gettext_noop("PC-FX HuEXE") },
 { NULL, NULL }
};

MDFNGI EmulatedPCFX =
{
 "pcfx",
 "PC-FX",
 KnownExtensions,
 MODPRIO_INTERNAL_HIGH,
 NULL,
 &PCFXInputInfo,
 NULL,
 NULL,
 LoadCD,
 TestMagicCD,
 CloseGame,
 KING_SetLayerEnableMask,
 "BG0\0BG1\0BG2\0BG3\0VDC-A BG\0VDC-A SPR\0VDC-B BG\0VDC-B SPR\0RAINBOW\0",
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 false,
 StateAction,
 Emulate,
 FXINPUT_SetInput,
 DoSimpleCommand,
 PCFXSettings,
 MDFN_MASTERCLOCK_FIXED(PCFX_MASTER_CLOCK),
 0,
 TRUE,  // Multires possible?

 0,   // lcm_width
 0,   // lcm_height
 NULL,  // Dummy

 288,	// Nominal width
 240,	// Nominal height

 1024,	// Framebuffer width
 512,	// Framebuffer height

 2,     // Number of output sound channels
};


static void set_basename(const char *path)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');

   if (base)
      retro_base_name = base + 1;
   else
      retro_base_name = path;

   retro_base_name = retro_base_name.substr(0, retro_base_name.find_last_of('.'));
}

#ifdef NEED_DEINTERLACER
static bool PrevInterlaced;
static Deinterlacer deint;
#endif

#define MEDNAFEN_CORE_NAME_MODULE "pcfx"
#define MEDNAFEN_CORE_NAME "Mednafen PC-FX"
#define MEDNAFEN_CORE_VERSION "v0.9.33.3"
#define MEDNAFEN_CORE_EXTENSIONS "cue|ccd|toc"
#define MEDNAFEN_CORE_TIMING_FPS 59.94
#define MEDNAFEN_CORE_GEOMETRY_BASE_W (game->nominal_width)
#define MEDNAFEN_CORE_GEOMETRY_BASE_H (game->nominal_height)
#define MEDNAFEN_CORE_GEOMETRY_MAX_W 341
#define MEDNAFEN_CORE_GEOMETRY_MAX_H 480
#define MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO (4.0 / 3.0)
#define FB_WIDTH 344
#define FB_HEIGHT 480

#define FB_MAX_HEIGHT FB_HEIGHT

const char *mednafen_core_str = MEDNAFEN_CORE_NAME;

static void check_system_specs(void)
{
   unsigned level = 15;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else 
      log_cb = NULL;

   MDFNI_InitializeModule();

   const char *dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      retro_base_directory = dir;
      // Make sure that we don't have any lingering slashes, etc, as they break Windows.
      size_t last = retro_base_directory.find_last_not_of("/\\");
      if (last != std::string::npos)
         last++;

      retro_base_directory = retro_base_directory.substr(0, last);

      MDFNI_Initialize(retro_base_directory.c_str());
   }
   else
   {
      /* TODO: Add proper fallback */
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "System directory is not defined. Fallback on using same dir as ROM for system directory later ...\n");
      failed_init = true;
   }
   
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
	  // If save directory is defined use it, otherwise use system directory
      retro_save_directory = *dir ? dir : retro_base_directory;
      // Make sure that we don't have any lingering slashes, etc, as they break Windows.
      size_t last = retro_save_directory.find_last_not_of("/\\");
      if (last != std::string::npos)
         last++;

      retro_save_directory = retro_save_directory.substr(0, last);      
   }
   else
   {
      /* TODO: Add proper fallback */
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "Save directory is not defined. Fallback on using SYSTEM directory ...\n");
	  retro_save_directory = retro_base_directory;
   }      

#if defined(WANT_16BPP) && defined(FRONTEND_SUPPORTS_RGB565)
   enum retro_pixel_format rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
      log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
#endif

   if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
      perf_get_cpu_features_cb = perf_cb.get_cpu_features;
   else
      perf_get_cpu_features_cb = NULL;

   check_system_specs();
}

void retro_reset(void)
{
   game->DoSimpleCommand(MDFN_MSC_RESET);
}

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{
   return false;
}

static void set_volume (uint32_t *ptr, unsigned number)
{
   switch(number)
   {
      default:
         *ptr = number;
         break;
   }
}

static void check_variables(void)
{
   struct retro_variable var = {0};

}

#define MAX_PLAYERS 2
#define MAX_BUTTONS 12
static uint16_t input_buf[MAX_PLAYERS];


static void hookup_ports(bool force)
{
   MDFNGI *currgame = game;

   if (initial_ports_hookup && !force)
      return;

   currgame->SetInput(0, "gamepad", &input_buf[0]);

   initial_ports_hookup = true;
}

bool retro_load_game(const struct retro_game_info *info)
{
   if (failed_init)
      return false;

#ifdef WANT_32BPP
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Pixel format XRGB8888 not supported by platform, cannot use %s.\n", MEDNAFEN_CORE_NAME);
      return false;
   }
#endif

   overscan = false;
   environ_cb(RETRO_ENVIRONMENT_GET_OVERSCAN, &overscan);

   set_basename(info->path);

   game = MDFNI_LoadGame(MEDNAFEN_CORE_NAME_MODULE, info->path);
   if (!game)
      return false;

   MDFN_PixelFormat pix_fmt(MDFN_COLORSPACE_RGB, 16, 8, 0, 24);
   memset(&last_pixel_format, 0, sizeof(MDFN_PixelFormat));
   
   surf = new MDFN_Surface(NULL, FB_WIDTH, FB_HEIGHT, FB_WIDTH, pix_fmt);

#ifdef NEED_DEINTERLACER
	PrevInterlaced = false;
	deint.ClearState();
#endif

   hookup_ports(true);

   check_variables();

   return game;
}

void retro_unload_game()
{
   if (!game)
      return;

   MDFNI_CloseGame();
}



// Hardcoded for PSX. No reason to parse lots of structures ...
// See mednafen/psx/input/gamepad.cpp
static void update_input(void)
{
   MDFNGI *currgame = (MDFNGI*)game;
   input_buf[0] = input_buf[1] = 0;
   static unsigned map[] = {
      RETRO_DEVICE_ID_JOYPAD_A,
      RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_X,
      RETRO_DEVICE_ID_JOYPAD_Y,
      RETRO_DEVICE_ID_JOYPAD_L,
      RETRO_DEVICE_ID_JOYPAD_R,
      RETRO_DEVICE_ID_JOYPAD_SELECT,
      RETRO_DEVICE_ID_JOYPAD_START,
      RETRO_DEVICE_ID_JOYPAD_UP,
      RETRO_DEVICE_ID_JOYPAD_RIGHT,
      RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT,
   };

   for (unsigned j = 0; j < MAX_PLAYERS; j++)
   {
      for (unsigned i = 0; i < MAX_BUTTONS; i++)
         input_buf[j] |= map[i] != -1u &&
            input_state_cb(j, RETRO_DEVICE_JOYPAD, 0, map[i]) ? (1 << i) : 0;

#ifdef MSB_FIRST
      union {
         uint8_t b[2];
         uint16_t s;
      } u;
      u.s = input_buf[j];
      input_buf[j] = u.b[0] | u.b[1] << 8;
#endif

   }
}

static uint64_t video_frames, audio_frames;


void retro_run()
{
   MDFNGI *curgame = game;

   input_poll_cb();

   update_input();

   static int16_t sound_buf[0x10000];
   static MDFN_Rect rects[FB_MAX_HEIGHT];
   rects[0].w = ~0;

   EmulateSpecStruct spec = {0};
   spec.surface = surf;
   spec.SoundRate = 44100;
   spec.SoundBuf = sound_buf;
   spec.LineWidths = rects;
   spec.SoundBufMaxSize = sizeof(sound_buf) / 2;
   spec.SoundVolume = 1.0;
   spec.soundmultiplier = 1.0;
   spec.SoundBufSize = 0;
   spec.VideoFormatChanged = false;
   spec.SoundFormatChanged = false;

   if (memcmp(&last_pixel_format, &spec.surface->format, sizeof(MDFN_PixelFormat)))
   {
      spec.VideoFormatChanged = TRUE;

      last_pixel_format = spec.surface->format;
   }

   if (spec.SoundRate != last_sound_rate)
   {
      spec.SoundFormatChanged = true;
      last_sound_rate = spec.SoundRate;
   }

   curgame->Emulate(&spec);

#ifdef NEED_DEINTERLACER
   if (spec.InterlaceOn)
   {
      if (!PrevInterlaced)
         deint.ClearState();

      deint.Process(spec.surface, spec.DisplayRect, spec.LineWidths, spec.InterlaceField);

      PrevInterlaced = true;

      spec.InterlaceOn = false;
      spec.InterlaceField = 0;
   }
   else
      PrevInterlaced = false;
#endif

   int16 *const SoundBuf = spec.SoundBuf + spec.SoundBufSizeALMS * curgame->soundchan;
   int32 SoundBufSize = spec.SoundBufSize - spec.SoundBufSizeALMS;
   const int32 SoundBufMaxSize = spec.SoundBufMaxSize - spec.SoundBufSizeALMS;

   spec.SoundBufSize = spec.SoundBufSizeALMS + SoundBufSize;

   unsigned width  = spec.DisplayRect.w;
   unsigned height = spec.DisplayRect.h;

#if defined(WANT_32BPP)
   const uint32_t *pix = surf->pixels;
   video_cb(pix, width, height, FB_WIDTH << 2);
#elif defined(WANT_16BPP)
   const uint16_t *pix = surf->pixels16;
   video_cb(pix, width, height, FB_WIDTH << 1);
#endif

   video_frames++;
   audio_frames += spec.SoundBufSize;

   audio_batch_cb(spec.SoundBuf, spec.SoundBufSize);

   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = MEDNAFEN_CORE_NAME;
   info->library_version  = MEDNAFEN_CORE_VERSION;
   info->need_fullpath    = true;
   info->valid_extensions = MEDNAFEN_CORE_EXTENSIONS;
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = MEDNAFEN_CORE_TIMING_FPS;
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = MEDNAFEN_CORE_GEOMETRY_BASE_W;
   info->geometry.base_height  = MEDNAFEN_CORE_GEOMETRY_BASE_H;
   info->geometry.max_width    = MEDNAFEN_CORE_GEOMETRY_MAX_W;
   info->geometry.max_height   = MEDNAFEN_CORE_GEOMETRY_MAX_H;
   info->geometry.aspect_ratio = MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO;
}

void retro_deinit()
{
   delete surf;
   surf = NULL;

   if (log_cb)
   {
      log_cb(RETRO_LOG_INFO, "[%s]: Samples / Frame: %.5f\n",
            mednafen_core_str, (double)audio_frames / video_frames);
      log_cb(RETRO_LOG_INFO, "[%s]: Estimated FPS: %.5f\n",
            mednafen_core_str, (double)video_frames * 44100 / audio_frames);
   }
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC; // FIXME: Regions for other cores.
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
   MDFNGI *currgame = (MDFNGI*)game;

   if (!currgame)
      return;

   switch(device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (currgame->SetInput)
            currgame->SetInput(in_port, "gamepad", &input_buf[in_port]);
         break;
      case RETRO_DEVICE_MOUSE:
         if (currgame->SetInput)
            currgame->SetInput(in_port, "mouse", &input_buf[in_port]);
         break;
   }
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   static const struct retro_controller_description pads[] = {
      { "PCFX Joypad", RETRO_DEVICE_JOYPAD },
      { "Mouse", RETRO_DEVICE_MOUSE },
   };

   static const struct retro_controller_info ports[] = {
      { pads, 2 },
      { pads, 2 },
      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

static size_t serialize_size;

size_t retro_serialize_size(void)
{
   MDFNGI *curgame = (MDFNGI*)game;
   //if (serialize_size)
   //   return serialize_size;

   if (!curgame->StateAction)
   {
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "[mednafen]: Module %s doesn't support save states.\n", curgame->shortname);
      return 0;
   }

   StateMem st;
   memset(&st, 0, sizeof(st));

   if (!MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL))
   {
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "[mednafen]: Module %s doesn't support save states.\n", curgame->shortname);
      return 0;
   }

   free(st.data);
   return serialize_size = st.len;
}

bool retro_serialize(void *data, size_t size)
{
   StateMem st;
   memset(&st, 0, sizeof(st));
   st.data     = (uint8_t*)data;
   st.malloced = size;

   return MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL);
}

bool retro_unserialize(const void *data, size_t size)
{
   StateMem st;
   memset(&st, 0, sizeof(st));
   st.data = (uint8_t*)data;
   st.len  = size;

   return MDFNSS_LoadSM(&st, 0, 0);
}

void *retro_get_memory_data(unsigned)
{
   return NULL;
}

size_t retro_get_memory_size(unsigned)
{
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned, bool, const char *)
{}

#ifdef _WIN32
static void sanitize_path(std::string &path)
{
   size_t size = path.size();
   for (size_t i = 0; i < size; i++)
      if (path[i] == '/')
         path[i] = '\\';
}
#endif

// Use a simpler approach to make sure that things go right for libretro.
std::string MDFN_MakeFName(MakeFName_Type type, int id1, const char *cd1)
{
   char slash;
#ifdef _WIN32
   slash = '\\';
#else
   slash = '/';
#endif
   std::string ret;
   switch (type)
   {
      case MDFNMKF_SAV:
         ret = retro_save_directory +slash + retro_base_name +
            std::string(".") +
#ifndef _XBOX
	    md5_context::asciistr(MDFNGameInfo->MD5, 0) + std::string(".") +
#endif
            std::string(cd1);
         break;
      case MDFNMKF_FIRMWARE:
         ret = retro_base_directory + slash + std::string(cd1);
#ifdef _WIN32
   sanitize_path(ret); // Because Windows path handling is mongoloid.
#endif
         break;
      default:	  
         break;
   }

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "MDFN_MakeFName: %s\n", ret.c_str());
   return ret;
}

void MDFND_DispMessage(unsigned char *str)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s\n", str);
}

void MDFND_Message(const char *str)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s\n", str);
}

void MDFND_MidSync(const EmulateSpecStruct *)
{}

void MDFN_MidLineUpdate(EmulateSpecStruct *espec, int y)
{
 //MDFND_MidLineUpdate(espec, y);
}

void MDFND_PrintError(const char* err)
{
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "%s\n", err);
}

void MDFND_Sleep(unsigned int time)
{
   retro_sleep(time);
}

#ifdef WANT_THREADING
MDFN_Thread *MDFND_CreateThread(int (*fn)(void *), void *data)
{
   return (MDFN_Thread*)sthread_create((void (*)(void*))fn, data);
}

void MDFND_WaitThread(MDFN_Thread *thr, int *val)
{
   sthread_join((sthread_t*)thr);

   if (val)
   {
      *val = 0;
      fprintf(stderr, "WaitThread relies on return value.\n");
   }
}

void MDFND_KillThread(MDFN_Thread *)
{
   fprintf(stderr, "Killing a thread is a BAD IDEA!\n");
}

MDFN_Mutex *MDFND_CreateMutex()
{
   return (MDFN_Mutex*)slock_new();
}

void MDFND_DestroyMutex(MDFN_Mutex *lock)
{
   slock_free((slock_t*)lock);
}

int MDFND_LockMutex(MDFN_Mutex *lock)
{
   slock_lock((slock_t*)lock);
   return 0;
}

int MDFND_UnlockMutex(MDFN_Mutex *lock)
{
   slock_unlock((slock_t*)lock);
   return 0;
}

MDFN_Cond *MDFND_CreateCond(void)
{
   return (MDFN_Cond*)scond_new();
}

void MDFND_DestroyCond(MDFN_Cond *cond)
{
   scond_free((scond_t*)cond);
}

int MDFND_WaitCond(MDFN_Cond *cond, MDFN_Mutex *mutex)
{
   scond_wait((scond_t*)cond, (slock_t*)mutex);
   return 0; // not sure about this return
}

int MDFND_SignalCond(MDFN_Cond *cond)
{
   scond_signal((scond_t*)cond);
   return 0; // not sure about this return
}
#endif
