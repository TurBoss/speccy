/*
 * Part of Jari Komppa's zx spectrum suite
 * https://github.com/jarikomppa/speccy
 * released under the unlicense, see http://unlicense.org 
 * (practically public domain)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screen_unpacker.h"
#include "lzfpack.h"
#include "tapper.h"
#include "fona.h"
#include "bootbin.h"

#define VERSION "1.0"

#define SPEC_Y(y)  (((y) & 0xff00) | ((((y) >> 0) & 7) << 3) | ((((y) >> 3) & 7) << 0) | (((y) >> 6) & 3) << 6)
#define SCR_LENGTH (32*192+24*32)

#define BOOTLOADER_OFS 0x21

#define PATCH_BOOTLOADER_POS 0x19
#define PATCH_DESTPOS 0x22
#define PATCH_COMPRESSEDLEN 0x26
#define PATCH_SOURCEPOS 0x2a
#define PATCH_DESTPOS2 0x12b

#define PATCH_BORDER 0x60
#define PATCH_CLEAR 0x0f

#define CODE_OFFSET (23759+69) // legal code offset, 23759+basic size

Tapper gLoaderHeader, gLoaderPayload;
Tapper gAppHeader, gAppPayload;
LZFPack gScreen, gApp;

int gExecAddr = 0;
int gBootExecAddr = 0;
int gMaxAddr = 0xffff;
char gProgName[11];

int gOptWeirdScr = 0;
int gOptNoBorder = 0;
int gOptNoClear = 0;
int gOptVerbose = 1;
int gOptBinary = 0;

void drawtext(unsigned char *aBuf, int aX, int aY, char *aText)
{
    int j=0;
    while (*aText)
    {
        int charofs = (*aText / 16) * 16 * 32 + (*aText % 16)*2;
        int i;
        for (i = 0; i < 16; i++)
        {
            aBuf[SPEC_Y(aY+i)*32+aX] = fona[charofs + i * 32] ^ 0xff;
            aBuf[SPEC_Y(aY+i)*32+aX+1] = fona[charofs+1 + i * 32] ^ 0xff;
        }
        aText++;
        aX+=2;
    }
}

void gen_basic()
{
/*
10 CLEAR 32767 : RANDOMIZE USR 23759 : POKE 23739,111 : LOAD ""CODE : RANDOMIZE USR 32768    

CLEAR 32767    
0xfd, 0x33, 0x32, 0x37, 0x36, 0x37, 0x0e, 0x00, 0x00, 0xff, 0x7f, 0x00, 0x0d, 
RANDOMIZE USR 23759
0xf9, 0xc0, 0x32, 0x33, 0x37, 0x35, 0x39, 0x0e, 0x00, 0x00, 0xcf, 0x5c, 0x00, 0x0d, 
POKE 23739,111
0xf4, 0x32, 0x33, 0x37, 0x33, 0x39, 0x0e, 0x00, 0x00, 0xbb, 0x5c, 0x00, 0x2c, 0x31, 0x31, 0x31, 0x0e, 0x00, 0x00, 0x6f, 0x00, 0x00, 0x0d, 
LOAD""CODE
0xef, 0x22, 0x22, 0xaf, 0x0d, 
RANDOMIZE USR 32768
0xf9, 0xc0, 0x33, 0x32, 0x37, 0x36, 0x38, 0x0e, 0x00, 0x00, 0x00, 0x80, 0x00, 0x0d, 
*/
    gLoaderPayload.putdata(0x00);
    gLoaderPayload.putdata(0x0a); // line number 10
    gLoaderPayload.putdata(69); // bytes on line (69)
    gLoaderPayload.putdata(0x00); // 0?
    gLoaderPayload.putdata(0xfd); // CLEAR
    gLoaderPayload.putdataintlit(gBootExecAddr-1);
    gLoaderPayload.putdata(':');
    gLoaderPayload.putdata(0xf9); // RANDOMIZE
    gLoaderPayload.putdata(0xc0); // USR
    gLoaderPayload.putdataintlit(CODE_OFFSET); // 23759+basic size
    gLoaderPayload.putdata(':');
    gLoaderPayload.putdata(0xf4); // POKE
    gLoaderPayload.putdataintlit(23739);
    gLoaderPayload.putdata(',');
    gLoaderPayload.putdataintlit(111);
    gLoaderPayload.putdata(':');
    gLoaderPayload.putdata(0xef); // LOAD
    gLoaderPayload.putdata('"');
    gLoaderPayload.putdata('"');
    gLoaderPayload.putdata(0xaf); // CODE
    gLoaderPayload.putdata(':');
    gLoaderPayload.putdata(0xf9); // RANDOMIZE
    gLoaderPayload.putdata(0xc0); // USR
    gLoaderPayload.putdataintlit(gBootExecAddr);
    gLoaderPayload.putdata(0x0d); // enter
    if (gOptVerbose) printf("BASIC part       : 69 bytes\n");
}

void append_screen_unpacker()
{
    screen_unpacker[6] = (gScreen.mMax >> 0) & 0xff;
    screen_unpacker[7] = (gScreen.mMax >> 8) & 0xff;
    int i;
    for (i = 0; i < screen_unpacker_len; i++)
        gLoaderPayload.putdata(screen_unpacker[i]);
    if (gOptVerbose) printf("Screen unpacker  : %d bytes\n", screen_unpacker_len);
}

void append_pic()
{ 
    int i;
    for (i = 0; i < gScreen.mMax; i++)
        gLoaderPayload.putdata(gScreen.mPackedData[i]);   
}

int checkpatch16(unsigned char *data, int ofs, int expected)
{
    if (boot_bin[ofs] != (expected & 0xff)) return 0;
    if (boot_bin[ofs+1] != ((expected >> 8) & 0xff)) return 0;
    return 1;
}

void patch16(unsigned char *data, int ofs, int patch, char * name)
{
    boot_bin[ofs] = patch & 0xff;
    boot_bin[ofs+1] = (patch >> 8) & 0xff;
}

void patch8(unsigned char *data, int ofs, int patch)
{
    boot_bin[ofs] = patch & 0xff;
}

void append_bootbin()
{
    if (gMaxAddr - (boot_bin_len + gApp.mMax) < CODE_OFFSET)
    {
        printf("Image doesn't fit in physical memory: 0x%04x - 0x%04x < 0x%04x\n", gMaxAddr, (boot_bin_len + gApp.mMax), CODE_OFFSET);
        exit(0);
    }
    if (gMaxAddr - (boot_bin_len + gApp.mMax) < gExecAddr)
    {
        printf("Compressed image bigger than uncompressed: 0x%04x - 0x%04x < %d\n", gMaxAddr, (boot_bin_len + gApp.mMax), gExecAddr);
        exit(0);
    }
    
    // sanity checks
    if (!checkpatch16(boot_bin, PATCH_DESTPOS, 0x6000)) { printf("patch position mismatch, aborting\n"); exit(0);}
    if (!checkpatch16(boot_bin, PATCH_DESTPOS2, 0x6000)) { printf("patch position mismatch, aborting\n"); exit(0);}
    if (!checkpatch16(boot_bin, PATCH_COMPRESSEDLEN, 0x2727)) { printf("patch position mismach, aborting\n"); exit(0);}
    if (!checkpatch16(boot_bin, PATCH_SOURCEPOS, 0xd000)) { printf("patch position mismatch, aborting\n"); exit(0);}
    
    int len = boot_bin_len + gApp.mMax;
    int image_ofs = gMaxAddr - len;
    int compressed_len = len - boot_bin_len;
    int dest_pos = gExecAddr;
    int source_pos = image_ofs + boot_bin_len;
    int bootloader_pos = image_ofs + BOOTLOADER_OFS;

    /*
    if (gOptVerbose) 
    {
        printf("Image offset   : 0x%04x (%d)\n", image_ofs, image_ofs);
        printf("Compressed len : 0x%04x (%d)\n", compressed_len, compressed_len);
        printf("Destination pos: 0x%04x (%d)\n", dest_pos, dest_pos);
        printf("Source pos     : 0x%04x (%d)\n", source_pos, source_pos);
        printf("Bootloader pos : 0x%04x (%d)\n", bootloader_pos, bootloader_pos);
    }
    */
    
    patch16(boot_bin, PATCH_BOOTLOADER_POS, bootloader_pos, "PATCH_BOOTLOADER_POS");
    patch16(boot_bin, PATCH_DESTPOS, dest_pos, "PATCH_DESTPOS");
    patch16(boot_bin, PATCH_DESTPOS2, dest_pos, "PATCH_DESTPOS2");
    patch16(boot_bin, PATCH_COMPRESSEDLEN, compressed_len, "PATCH_COMPRESSEDLEN");
    patch16(boot_bin, PATCH_SOURCEPOS, source_pos, "PATCH_SOURCEPOS");

    if (gOptNoBorder)
    {
        patch8(boot_bin, PATCH_BORDER, 0);
        patch8(boot_bin, PATCH_BORDER+1, 0);
    }
    
    if (gOptNoClear)
    {
        patch8(boot_bin, PATCH_CLEAR, 0);
        patch8(boot_bin, PATCH_CLEAR+1, 0);
        patch8(boot_bin, PATCH_CLEAR+2, 0);
    }    
    
    int i;
    for (i = 0; i < boot_bin_len; i++)
        gAppPayload.putdata(boot_bin[i]);
        
    if (gOptVerbose) printf("App bootstrap    : %d bytes\n", boot_bin_len);
}

void append_app()
{ 
    int i;
    for (i = 0; i < gApp.mMax; i++)
        gAppPayload.putdata(gApp.mPackedData[i]);   
}

void save_tap(char *aFilename)
{
    FILE * f = fopen(aFilename, "wb");
    if (!f)
    {
        printf("Can't open \"%s\" for writing.\n", aFilename);
        exit(0);
    }
    gLoaderHeader.putdata((unsigned char)0); // 0 = program
    gLoaderHeader.putdatastr(gProgName); // 10 chars exact
    gLoaderHeader.putdataint(gLoaderPayload.ofs-1);
    gLoaderHeader.putdataint(10); // autorun row
    gLoaderHeader.putdataint(gLoaderPayload.ofs-1);
    
    gLoaderHeader.write(f);
    gLoaderPayload.write(f);
    
    gAppHeader.putdata((unsigned char)3); // 3 = code
    gAppHeader.putdatastr("iki.fi/sol"); // 10 chars exact (pretty much nobody will see this)
    gAppHeader.putdataint(gAppPayload.ofs-1);
    gAppHeader.putdataint(gBootExecAddr); // "Start of the code block when saved"
    gAppHeader.putdataint(32768);
    
    gAppHeader.write(f);
    gAppPayload.write(f);
    
    int len = ftell(f);
    fclose(f);
    if (gOptVerbose) 
        printf("\"%s\" written: %d bytes\n"
        "Estimated load time: %d seconds (%d secs to loading screen).\n", 
        aFilename, 
        len, 
        len * 8 / 1200 + 10, 
        (gScreen.mMax + 69) * 8 / 1200 + 6);
}

int decode_ihx(unsigned char *src, int len, unsigned char *data)
{
    int start = 0x10000;
    int end = 0;
    int line = 0;
    int idx = 0;
    while (src[idx])
    {
        line++;
        int sum = 0;
        char tmp[8];
        if (src[idx] != ':')
        {
            printf("Parse error near line %d? (previous chars:\"%c%c%c%c%c%c\")\n", line,
                (idx<5)?'?':(src[idx-5]<32)?'?':src[idx-5],
                (idx<4)?'?':(src[idx-4]<32)?'?':src[idx-4],
                (idx<3)?'?':(src[idx-3]<32)?'?':src[idx-3],
                (idx<2)?'?':(src[idx-2]<32)?'?':src[idx-2],
                (idx<1)?'?':(src[idx-1]<32)?'?':src[idx-1],
                (idx<0)?'?':(src[idx-0]<32)?'?':src[idx-0]);
            exit(0);
        }
        idx++;
        tmp[0] = '0';
        tmp[1] = 'x';
        tmp[2] = src[idx]; idx++;
        tmp[3] = src[idx]; idx++;
        tmp[4] = 0;
        int bytecount = strtol(tmp,0,16);
        sum += bytecount;
        tmp[2] = src[idx]; idx++;
        tmp[3] = src[idx]; idx++;
        tmp[4] = src[idx]; idx++;
        tmp[5] = src[idx]; idx++;
        tmp[6] = 0;
        int address = strtol(tmp,0,16);
        sum += (address >> 8) & 0xff;
        sum += (address & 0xff);
        tmp[2] = src[idx]; idx++;
        tmp[3] = src[idx]; idx++;
        tmp[4] = 0;
        int recordtype = strtol(tmp,0,16);
        sum += recordtype;
        switch (recordtype)
        {
            case 0:
            case 1:
                break;
            default:
                printf("Unsupported record type %d\n", recordtype);
                exit(0);
                break;
        }
        //printf("%d bytes from %d, record %d\n", bytecount, address, recordtype);
        while (bytecount)
        {
            tmp[2] = src[idx]; idx++;
            tmp[3] = src[idx]; idx++;
            tmp[4] = 0;
            int byte = strtol(tmp,0,16);
            sum += byte;
            data[address] = byte;
            if (start > address)
                start = address;
            if (end < address)
                end = address;
            address++;
            bytecount--;
        }
        tmp[2] = src[idx]; idx++;
        tmp[3] = src[idx]; idx++;
        tmp[4] = 0;
        int checksum = strtol(tmp,0,16);
        sum = (sum ^ 0xff) + 1;
        if (checksum != (sum & 0xff))
        {
            printf("Checksum failure %02x, expected %02x\n", sum & 0xff, checksum);
            exit(0);
        }

        while (src[idx] == '\n' || src[idx] == '\r') idx++;         
    }    
    gExecAddr = start;
    return end-start+1;
}

void load_code(char *aFilename)
{   
    unsigned char *data = new unsigned char[0x10000];
    memset(data, 0, 0x10000);
    FILE * f = fopen(aFilename, "rb");
    if (!f)
    {
        printf("\"%s\" not found\n", aFilename);
        exit(0);
    }
    fseek(f,0,SEEK_END);
    int len = ftell(f);
    fseek(f,0,SEEK_SET);
    
    unsigned char * src = new unsigned char[len+1];
    
    fread(src, len, 1, f);
    fclose(f);
    src[len] = 0;
    int image_size;

    if (gOptBinary)
    {
        image_size = len;
        memcpy(data + gExecAddr, src, len);
    }
    else
    {            
        image_size = decode_ihx(src, len, data);
    }
    delete[] src;
    gApp.pack(data + gExecAddr, image_size);
    if (gOptVerbose)
    printf(
        "\n\"%s\":\n"
        "\tExec address : %d (0x%x)\n"
        "\tImage size   : %d bytes\n"
        "\tCompressed to: %d bytes (%3.3f%%)\n", 
        aFilename, 
        gExecAddr, gExecAddr,
        image_size, 
        gApp.mMax, gApp.mMax*100.0f/image_size);
    
    delete[] data;
}

void set_progname(char *aName)
{
    int i;
    for (i = 0; i < 10; i++)
    {
        gProgName[i] = ' ';
    }
    
    gProgName[10] = 0;
    
    for (i = 0; i < 10 && aName[i]; i++)
    {
        gProgName[i] = aName[i];
    }
    
    if (gOptVerbose) printf("Progname set to \"%s\"\n", gProgName);
}


void load_loadingscreen(int aHaveit, char * aFilename)
{
    if (aHaveit)
    {
        FILE * f = fopen(aFilename, "rb");
        if (!f)
        {
            printf("\"%s\" not found\n", aFilename);
            exit(0);
        }
        fseek(f,0,SEEK_END);
        int len = ftell(f);
        if (gOptWeirdScr == 0 && len != SCR_LENGTH)
        {
            printf("\"%s\" size not %d - invalid .scr file?\n", aFilename, SCR_LENGTH);
            exit(0);
        }
        fseek(f,0,SEEK_SET);
        unsigned char *temp = new unsigned char[len];
        fread(temp, 1, len, f);
        fclose(f);
        gScreen.pack(temp, len);
        if (gOptVerbose)
        printf(
            "\n\"%s\"\n"
            "\tImage size   : %d bytes\n"
            "\tCompressed to: %d bytes (%3.3f%%)\n", 
            aFilename, len, gScreen.mMax, gScreen.mMax*100.0f/len);
        delete[] temp;
    }
    else
    {
        unsigned char *temp = new unsigned char[SCR_LENGTH];
        memset(temp, 0, 32*192);
        memset(temp + 32*192, 7 | (1 << 6), 11*32);       
        memset(temp + 32*192 + 11*32, 1 << 3, 32);       
        memset(temp + 32*192 + 12*32, 7, 32*12);
        drawtext(temp, 2, 8*8, "Loading");
        int l = 0;
        while (gProgName[l] != ' ' && l < 10) l++;
        drawtext(temp, 30-(l*2), 13*8, gProgName);        
        gScreen.pack(temp, SCR_LENGTH);
        if (gOptVerbose)
        printf(
            "\nGenerated loading screen\n"
            "\tImage size   : %d bytes\n"
            "\tCompressed to: %d bytes (%3.3f%%)\n", 
            SCR_LENGTH, gScreen.mMax, gScreen.mMax*100.0f/SCR_LENGTH);
        delete[] temp;        
    }
}

void print_usage(int aDo, char *aFilename)
{
    if (gOptVerbose && !aDo) printf("Mackarel " VERSION " by Jari Komppa, http://iki.fi/sol\n");
    
    if (aDo)
    {
        printf(
            "Generate zx spectrum tape files from intel hex files\n"
            "(with compressed loading screens and compressed data images)\n"
            "\n"
            "Usage:\n"
            "\n"
            "%s IHXFILE OUTFILENAME [APPNAME] [LOADINGSCREEN] [OPTIONS]\n"
            "\n"
            "Where:\n"
            "IHXFILE          - .ihx file generated by compiler/linker\n"
            "OUTFILENAME      - name of .tap file to generate\n"
            "APPNAME          - application name, max 10 chars, optional\n"
            "LOADINGSCREEN    - .scr file to use as loading screen, optional\n"
            "Options:\n"
            "-noborders       - Don't blink borders while unpacking\n"
            "-noclear         - Don't clear attributes to 0 before unpacking\n"
            "-maxaddress addr - Maximum address to overwrite, default 0x%04x\n"            
            "-binimage addr   - Input is not ihx but binary file. Needs exec addr.\n"
            "-weirdscr        - Ignore .scr file size, use whatever it is.\n"
            "-q               - Quiet mode, only print errors.\n"
            "\n",
            aFilename,
            gMaxAddr);
        exit(0);
    }
}

void parse_commandline(int parc, char ** pars)
{
    int i;
    for (i = 0; i < parc; i++)
    {
        if (pars[i][0] == '-')
        {
            if (stricmp(pars[i], "-weirdscr") == 0)
            {
                gOptWeirdScr = 1;
            }
            else
            if (stricmp(pars[i], "-noborders") == 0)
            {
                gOptNoBorder = 1;
            }
            else
            if (stricmp(pars[i], "-noclear") == 0)
            {
                gOptNoClear = 1;
            }
            else
            if (stricmp(pars[i], "-q") == 0)
            {
                gOptVerbose = 0;
            }
            else
            if (stricmp(pars[i], "-maxaddress") == 0)
            {
                i++;
                gMaxAddr = strtol(pars[i],0,0);
            }
            else
            if (stricmp(pars[i], "-binimage") == 0)
            {
                i++;
                gExecAddr = strtol(pars[i],0,0);
                gOptBinary = 1;
            }
            else
            {
                printf("Unknown option parameter %s", pars[i]);
                exit(0);
            }
        }
    }
        
}

int main(int parc, char ** pars)
{
    int nonflagparc = 0;
    int i;
    for (i = 0; i < parc && pars[i][0] != '-'; i++)
        nonflagparc++;
    
    print_usage(nonflagparc < 3, pars[0]);
    parse_commandline(parc, pars);
    set_progname(nonflagparc > 3 ? pars[3] : pars[1]);
    load_code(pars[1]);    
    load_loadingscreen(nonflagparc > 4, pars[4]);

    gBootExecAddr = gMaxAddr - (gApp.mMax + boot_bin_len);
    if (gOptVerbose)
    printf("Boot exec address: %d (0x%x)\n", gBootExecAddr, gBootExecAddr);
    
    // Block flag bytes
    gLoaderHeader.putdata((unsigned char)0x00);
    gLoaderPayload.putdata((unsigned char)0xff);
    gAppHeader.putdata((unsigned char)0x00);
    gAppPayload.putdata((unsigned char)0xff);
    
    // Loader
    gen_basic();
    append_screen_unpacker();
    append_pic();
    
    // App
    append_bootbin();    
    append_app();    
        
    save_tap(pars[2]);    
}    
