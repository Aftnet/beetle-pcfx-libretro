/*
 ===================================================================================
		Cygne WIN  v 2.1a      (c) Dox 2002     dox@space.pl
 ===================================================================================

   NEC cpu core  by Bryan McPhail,Oliver Bergmann, Fabrice Frances and David Hedley
   Zlib  by  Jean-loup Gailly and Mark Adler

 ===================================================================================
*/

const uint8 startio[256]={
0x00,//0
0x00,//1
0x9d,//2
0xbb,//3
0x00,//4
0x00,//5
0x00,//6
0x26,//7
0xfe,//8
0xde,//9
0xf9,//a
0xfb,//b
0xdb,//c
0xd7,//d
0x7f,//e
0xf5,//f
0x00,//10
0x00,//11
0x00,//12
0x00,//13
0x01,//14
0x00,//15
0x9e,//16
0x9b,//17
0x00,//18
0x00,//19
0x00,//1a
0x00,//1b
0x99,//1c
0xfd,//1d
0xb7,//1e
0xdf,//1f
0x30,//20
0x57,//21
0x75,//22
0x76,//23
0x15,//24
0x73,//25
0x77,//26
0x77,//27
0x20,//28
0x75,//29
0x50,//2a
0x36,//2b
0x70,//2c
0x67,//2d
0x50,//2e
0x77,//2f
0x57,//30
0x54,//31
0x75,//32
0x77,//33
0x75,//34
0x17,//35
0x37,//36
0x73,//37
0x50,//38
0x57,//39
0x60,//3a
0x77,//3b
0x70,//3c
0x77,//3d
0x10,//3e
0x73,//3f
0x00,//40
0x00,//41
0x00,//42
0x00,//43
0x00,//44
0x00,//45
0x00,//46
0x00,//47
0x00,//48
0x00,//49
0x00,//4a
0x00,//4b
0x00,//4c
0x00,//4d
0x00,//4e
0x00,//4f
0x00,//50
0x00,//51
0x00,//52
0x00,//53
0x00,//54
0x00,//55
0x00,//56
0x00,//57
0x00,//58
0x00,//59
0x00,//5a
0x00,//5b
0x00,//5c
0x00,//5d
0x00,//5e
0x00,//5f
0x0a,//60
0x00,//61
0x00,//62
0x00,//63
0x00,//64
0x00,//65
0x00,//66
0x00,//67
0x00,//68
0x00,//69
0x00,//6a
0x0f,//6b
0x00,//6c
0x00,//6d
0x00,//6e
0x00,//6f
0x00,//70
0x00,//71
0x00,//72
0x00,//73
0x00,//74
0x00,//75
0x00,//76
0x00,//77
0x00,//78
0x00,//79
0x00,//7a
0x00,//7b
0x00,//7c
0x00,//7d
0x00,//7e
0x00,//7f
0x00,//80
0x00,//81
0x00,//82
0x00,//83
0x00,//84
0x00,//85
0x00,//86
0x00,//87
0x00,//88
0x00,//89
0x00,//8a
0x00,//8b
0x00,//8c
0x1f,//8d 1d ?
0x00,//8e
0x00,//8f
0x00,//90
0x00,//91
0x00,//92
0x00,//93
0x00,//94
0x00,//95
0x00,//96
0x00,//97
0x00,//98
0x00,//99
0x00,//9a
0x00,//9b
0x00,//9c
0x00,//9d
0x03,//9e
0x00,//9f
0x87-2,//a0
0x00,//a1
0x00,//a2
0x00,//a3
0x0,//a4 2b
0x0,//a5 7f
0x4f,//a6
0xff,//a7 cf ?
0x00,//a8
0x00,//a9
0x00,//aa
0x00,//ab
0x00,//ac
0x00,//ad
0x00,//ae
0x00,//af
0x00,//b0
0xdb,//b1
0x00,//b2
0x00,//b3
0x00,//b4
0x40,//b5
0x00,//b6
0x00,//b7
0x00,//b8
0x00,//b9
0x01,//ba
0x00,//bb
0x42,//bc
0x00,//bd
0x83,//be
0x00,//bf
0x2f,//c0
0x3f,//c1
0xff,//c2
0xff,//c3
0x00,//c4
0x00,//c5
0x00,//c6
0x00,//c7

0xd1,//c8?
0xd1,//c9
0xd1,//ca
0xd1,//cb
0xd1,//cc
0xd1,//cd
0xd1,//ce
0xd1,//cf
0xd1,//d0
0xd1,//d1
0xd1,//d2
0xd1,//d3
0xd1,//d4
0xd1,//d5
0xd1,//d6
0xd1,//d7
0xd1,//d8
0xd1,//d9
0xd1,//da
0xd1,//db
0xd1,//dc
0xd1,//dd
0xd1,//de
0xd1,//df
0xd1,//e0
0xd1,//e1
0xd1,//e2
0xd1,//e3
0xd1,//e4
0xd1,//e5
0xd1,//e6
0xd1,//e7
0xd1,//e8
0xd1,//e9
0xd1,//ea
0xd1,//eb
0xd1,//ec
0xd1,//ed
0xd1,//ee
0xd1,//ef
0xd1,//f0
0xd1,//f1
0xd1,//f2
0xd1,//f3
0xd1,//f4
0xd1,//f5
0xd1,//f6
0xd1,//f7
0xd1,//f8
0xd1,//f9
0xd1,//fa
0xd1,//fb
0xd1,//fc
0xd1,//fd
0xd1,//fe
0xd1};//ff