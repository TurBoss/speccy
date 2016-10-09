/*
 * Part of Jari Komppa's zx spectrum suite
 * https://github.com/jarikomppa/speccy
 * released under the unlicense, see http://unlicense.org
 * (practically public domain)
*/

#include <string.h>

unsigned char fbcopy_idx;

unsigned char *data_ptr;
unsigned char *screen_ptr;

unsigned char port254tonebit;

#define FONTHEIGHT 8
#define COLOR(BLINK, BRIGHT, PAPER, INK) (((BLINK) << 7) | ((BRIGHT) << 6) | ((PAPER) << 3) | (INK))

#include "yofstab.h"
#define HWIF_IMPLEMENTATION
#include "hwif.c"
#include "propfont.h"
#include "drawstring.c"

//extern void zx7_unpack(unsigned char *src, unsigned char *dst) __z88dk_callee __z88dk_fastcall;
extern void zx7_unpack(unsigned char *src)  __z88dk_fastcall;


enum opcodeval
{
    OP_HAS,
    OP_NOT,
    OP_SET,
    OP_CLR,
    OP_XOR,
    OP_RND,
    OP_ATTR,
    OP_EXT
};

unsigned char state[256]; // game state
unsigned char y8; // prg state
unsigned char *answer[16];
unsigned char answers;
unsigned char attrib;

unsigned char * unpack_resource(unsigned short id)
{
    // dest = 0xd000, ~4k of scratch. A bit tight?
    
    unsigned short v;

    for (v = 0; v < 4096; v++)
        *((unsigned char*)0xd000 + v) = 0;

    v = (((unsigned short)*(unsigned char*)(0x5b00 + id*2+1)) << 8) | *(unsigned char*)(0x5b00 + id*2);
    zx7_unpack((unsigned char*)v);
    

    return (unsigned char*)0xd000;
}

unsigned char xorshift8(void) 
{
    y8 ^= (y8 << 7);
    y8 ^= (y8 >> 5);
    return y8 ^= (y8 << 3);
}

unsigned char get_bit(unsigned short id)
{
    return !!(state[id & 0xff] & 1 << (id >> 8));
}

void set_bit(unsigned short id)
{
    state[id & 0xff] |= 1 << (id >> 8);
}

void clear_bit(unsigned short id)
{
    state[id & 0xff] &= (1 << (id >> 8))^0xff;
}

void toggle_bit(unsigned short id)
{
    state[id & 0xff] ^= 1 << (id >> 8);
}

void set_attr(unsigned short id)
{
    attrib = id;
}

void set_ext(unsigned short id)
{
    if (id < 8)
    {
        port254(id);
    }
    // todo: sound?
}


void exec(unsigned char *dataptr)
{
    unsigned char c = *dataptr;
    
    switch (dataptr[1])
    {
        case 'I':
        case 'A':
        case 'Q': 
            dataptr += 1+1+2; 
            c -= 3; 
            break;
        case 'O': 
            dataptr += 1+1; 
            c -= 1; 
            break;
    }
    
    while (c)
    {
        // ignore predicate ops
        unsigned short id = ((unsigned short)dataptr[2] << 8) | dataptr[1];
        switch (*dataptr)
        {
            case OP_SET:
                set_bit(id);
                break;
            case OP_CLR:
                clear_bit(id);
                break;
            case OP_XOR:
                toggle_bit(id);
                break;
            case OP_ATTR:
                set_attr(id);
                break;
            case OP_EXT:
                set_ext(id);
                break;
        }
        dataptr += 3;
        c -= 3;
    }    
}

unsigned char pred(unsigned char *dataptr)
{
    unsigned char c = *dataptr;
    unsigned char ret = 1;
    
    switch (dataptr[1])
    {
        case 'I':
        case 'A':
        case 'Q':
            dataptr += 1+1+2; 
            c -= 3; 
            break; 
        case 'O': 
            dataptr += 1+1; 
            c -= 1; 
            break;
    }
    
    while (c)
    {
        // ignore exec ops
        unsigned short id = ((unsigned short)dataptr[2] << 8) | dataptr[1];
        
        switch (*dataptr)
        {
            case OP_HAS:
                if (!get_bit(id)) ret = 0;
                break;
            case OP_NOT:
                if (get_bit(id)) ret = 0;
                break;
            case OP_RND:
                if (xorshift8() > id) ret = 0;
                break;
        }
        
        dataptr += 3;
        c -= 3;
    }
    return ret;
}

void add_answer(unsigned char *dataptr)
{
    answer[answers] = dataptr;
    answers++;
    if (answers == 16)
        answers = 15;
}

void hitkeytocontinue()
{
    drawstring("[Press enter to continue]", 3, 21*8);
    
    readkeyboard();            
    while (KEYUP(ENTER))
    {
        readkeyboard();
    }            
    while (KEYDOWN(ENTER))
    {
        readkeyboard();
    }                
}

void cls()
{
    unsigned short i;
    for (i = 0; i < 192*32; i++)
      *(unsigned char*)(0x4000+i) = 0;
}

void image(unsigned char *dataptr, unsigned char *aYOfs)
{
    unsigned short id = ((unsigned short)dataptr[3] << 8) | dataptr[2];
    unsigned short yp;
    unsigned char x, y;
    
    dataptr = unpack_resource(id);
    id = *dataptr;
    yp = *aYOfs;
    
    if (id + yp > 20*8) 
    {
        hitkeytocontinue();
        cls();
        yp = 0;
    }
    
    dataptr++;
    for (y = 0; y < id; y++, yp++)
    {
        unsigned char * dst = (unsigned char*)yofs[yp];
        for (x = 0; x < 32; x++)
        {
            *dst = *dataptr;
            dataptr++;
            dst++;
        }
    }        
    *aYOfs = yp;
}


void render_room(unsigned short room_id)
{
    unsigned char *dataptr = unpack_resource(room_id);
    unsigned char output_enable = 1;
    unsigned char yofs = 0;

    set_bit(room_id);

    cls();
    
    while (*dataptr)
    {       
        switch (dataptr[1])
        {
            case 'Q': 
                if (!output_enable) // the next room
                {
                    return;
                }
                if (pred(dataptr)) 
                {
                    exec(dataptr); 
                }
                answers = 0;
                break;
            case 'I': 
                if (pred(dataptr))
                {
                    exec(dataptr);
                    image(dataptr, &yofs); 
                    unpack_resource(room_id); // re-load the room data
                }
                break;
            case 'O': 
                if (pred(dataptr)) 
                {
                    exec(dataptr);
                    output_enable = 1;
                }
                else
                {
                    output_enable = 0;
                }
                break;
            case 'A':
                if (pred(dataptr))
                {
                    add_answer(dataptr);
                }
                output_enable = 0;
                break;
        }
        dataptr += *dataptr + 1;
        
        while (*dataptr)
        {
            if (output_enable)
            {
                drawstring_lr_pascal(dataptr, 0, yofs);
                yofs += 8;
                if (yofs > 20*8) 
                {
                    hitkeytocontinue();
                    cls();
                    yofs = 0;                
                }
            }
            dataptr += *dataptr + 1;
        }
        dataptr++;
    }
    // if we get here, this was the last room in the data
}

void clearbottom()
{
    unsigned short i, j;

    for (j = 20*8; j < 24*8; j++)
        for (i = 0; i < 32; i++)
            *(unsigned char*)(yofs[j]+i) = 0;
}


void reset()
{
    unsigned short i;

    for (i = 0; i < 256; i++)
        state[i] = 0;

    cls();
    
    for (i = 0; i < 24*32; i++)
        *(unsigned char*)(0x5800+i) = 7 << 3; 

    port254(7);

    y8 = 1;
    answers = 0;
    attrib = 7 << 3;
}

void main()
{
    unsigned short i;
    unsigned short current_room = 0;    
    unsigned char current_answer = 0;
    unsigned char selecting;

    reset();     
        
    while(1)
    { 
        /*
        - code string
        - 0..n strings
        - 0        
        */
        
        render_room(current_room);

        if (answers == 0)
        {
            unsigned short t;
            clearbottom();
            // delay..
            for (t = 0; t < 30000; t++);
            for (t = 0; t < 30000; t++);
            for (t = 0; t < 30000; t++);
            for (t = 0; t < 30000; t++);
            
            drawstring("[The end. Press enter to restart]", 3, 21*8);
            
            readkeyboard();            
            while (KEYUP(ENTER))
            {
                readkeyboard();
            }            
            while (KEYDOWN(ENTER))
            {
                readkeyboard();
            }            
            
            current_room = 0;
            current_answer = 0;
            reset();            
        }
        else
        {
    
            if (current_answer >= answers)
                current_answer = 0;
                
            selecting = 1;
            while (selecting)
            {
                clearbottom();
                for (i = 0; i < 3; i++)
                {
                    if (current_answer + i < answers)
                    {
                        unsigned char *dataptr = answer[current_answer + i];
                        unsigned char roll = 0;
                        dataptr += *dataptr + 1;
                        drawstring_lr_pascal(dataptr, 3, 21*8 + i * 8);
                    }
                }
                                                    
                readkeyboard();
                while (KEYUP(Q) && KEYUP(A) && KEYUP(ENTER))
                {
                    drawstring(">>>", 0, 21*8);
                    
                    readkeyboard();
                }
                if (KEYDOWN(ENTER))
                {
                    unsigned char *dataptr = answer[current_answer];
                    unsigned short id = ((unsigned short)dataptr[3] << 8) | dataptr[2];
                    while (KEYDOWN(ENTER))
                    {
                        readkeyboard();
                    }
                    selecting = 0;
                    exec(answer[current_answer]);
                    if (current_room != id)
                        current_answer = 0;
                    current_room = id;
                }
                if (KEYDOWN(Q))
                {
                    while (KEYDOWN(Q))
                    {
                        readkeyboard();
                    }
                    if (current_answer > 0)
                        current_answer--;
                }
                if (KEYDOWN(A))
                {
                    while (KEYDOWN(A))
                    {
                        readkeyboard();
                    }
                    if (current_answer+1 < answers)
                        current_answer++;
                }
                xorshift8();    
            }
        }
    }
}
