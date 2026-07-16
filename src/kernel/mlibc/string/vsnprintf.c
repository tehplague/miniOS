// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <miniOS/drivers/console.h>
#include <miniOS/io.h>
#include <miniOS/types.h>
#include <string.h>

char* __int_str(intmax_t i, char b[], int base, bool plusSignIfNeeded, bool spaceSignIfNeeded,
                int paddingNo, bool justify, bool zeroPad) {

    char digit[32] = {0};
    memset(digit, 0, 32);
    strcpy(digit, "0123456789");

    if (base == 16) {
        strcat(digit, "ABCDEF");
    } else if (base == 17) {
        strcat(digit, "abcdef");
        base = 16;
    }

    char* p = b;
    if (i < 0) {
        *p++ = '-';
        i *= -1;
    } else if (plusSignIfNeeded) {
        *p++ = '+';
    } else if (!plusSignIfNeeded && spaceSignIfNeeded) {
        *p++ = ' ';
    }

    intmax_t shifter = i;
    do {
        ++p;
        shifter = shifter / base;
    } while (shifter);

    *p = '\0';
    do {
        *--p = digit[i % base];
        i = i / base;
    } while (i);

    int padding = paddingNo - (int)strlen(b);
    if (padding < 0) padding = 0;

    if (justify) {
        while (padding--) {
            if (zeroPad) {
                b[strlen(b)] = '0';
            } else {
                b[strlen(b)] = ' ';
            }
        }

    } else {
        char a[256] = {0};
        while (padding--) {
            if (zeroPad) {
                a[strlen(a)] = '0';
            } else {
                a[strlen(a)] = ' ';
            }
        }
        strcat(a, b);
        strcpy(b, a);
    }

    return b;
}

char* __uint_str(uintmax_t i, char b[], int base, bool plusSignIfNeeded,
                 bool spaceSignIfNeeded, int paddingNo, bool justify, bool zeroPad) {

    char digit[32] = {0};
    memset(digit, 0, 32);
    strcpy(digit, "0123456789");

    if (base == 16) {
        strcat(digit, "ABCDEF");
    } else if (base == 17) {
        strcat(digit, "abcdef");
        base = 16;
    }

    char* p = b;
    if (plusSignIfNeeded) {
        *p++ = '+';
    } else if (spaceSignIfNeeded) {
        *p++ = ' ';
    }

    uintmax_t shifter = i;
    do {
        ++p;
        shifter = shifter / base;
    } while (shifter);

    *p = '\0';
    do {
        *--p = digit[i % base];
        i = i / base;
    } while (i);

    int padding = paddingNo - (int)strlen(b);
    if (padding < 0) padding = 0;

    if (justify) {
        while (padding--) {
            if (zeroPad) {
                b[strlen(b)] = '0';
            } else {
                b[strlen(b)] = ' ';
            }
        }
    } else {
        char a[256] = {0};
        while (padding--) {
            if (zeroPad) {
                a[strlen(a)] = '0';
            } else {
                a[strlen(a)] = ' ';
            }
        }
        strcat(a, b);
        strcpy(b, a);
    }

    return b;
}

void displayCharacter(char c, int* a) {
    putchar(c);
    *a += 1;
}

void displayString(char* c, int* a) {
    for (int i = 0; c[i]; ++i) {
        displayCharacter(c[i], a);
    }
}

int vsnprintf (char *str, size_t size, const char* format, va_list list) {
#define OUTPUTCHAR(c)      \
  do {                     \
    *str++ = (c);          \
    chars++;               \
    if (chars >= size-1) { \
      *str = '\0';         \
      chars++;             \
      return chars;        \
    }                      \
  } while(0)
#define OUTPUTSTRING(s)    \
  do {                     \
    const char *_s = s;    \
    while (*_s != '\0') {  \
        OUTPUTCHAR(*_s++); \
    }                      \
  } while(0)

    int chars        = 0;
    char intStrBuffer[256] = {0};

    for (int i = 0; format[i]; ++i) {
        char specifier   = '\0';
        char length      = '\0';

        int  lengthSpec  = 0;
        int  precSpec    = 0;
        bool leftJustify = false;
        bool zeroPad     = false;
        bool spaceNoSign = false;
        bool altForm     = false;
        bool plusSign    = false;
        int  expo        = 0;

        if (format[i] == '%') {
            ++i;

            bool extBreak = false;
            while (1) {

                switch (format[i]) {
                    case '-':
                        leftJustify = true;
                        ++i;
                        break;

                    case '+':
                        plusSign = true;
                        ++i;
                        break;

                    case '#':
                        altForm = true;
                        ++i;
                        break;

                    case ' ':
                        spaceNoSign = true;
                        ++i;
                        break;

                    case '0':
                        zeroPad = true;
                        ++i;
                        break;

                    default:
                        extBreak = true;
                        break;
                }

                if (extBreak) break;
            }

            while (isdigit(format[i])) {
                lengthSpec *= 10;
                lengthSpec += format[i] - 48;
                ++i;
            }

            if (format[i] == '*') {
                lengthSpec = va_arg(list, int);
                ++i;
            }

            if (format[i] == '.') {
                ++i;
                while (isdigit(format[i])) {
                    precSpec *= 10;
                    precSpec += format[i] - 48;
                    ++i;
                }

                if (format[i] == '*') {
                    precSpec = va_arg(list, int);
                    ++i;
                }
            } else {
                precSpec = 6;
            }

            if (format[i] == 'h' || format[i] == 'l' || format[i] == 'j' ||
                format[i] == 'z' || format[i] == 't' || format[i] == 'L') {
                length = format[i];
                ++i;
                if (format[i] == 'h') {
                    length = 'H';
                } else if (format[i] == 'l') {
                    length = 'q';
                    ++i;
                }
            }
            specifier = format[i];

            memset(intStrBuffer, 0, 256);

            int base = 10;
            if (specifier == 'o') {
                base = 8;
                specifier = 'u';
                if (altForm) {
                    OUTPUTSTRING("0");
                }
            }
            if (specifier == 'p') {
                base = 16;
                length = 'z';
                specifier = 'u';
            }
            switch (specifier) {
                case 'X':
                    base = 16;
                case 'x':
                    base = base == 10 ? 17 : base;
                    if (altForm) {
                        OUTPUTSTRING("0x");
                    }

                case 'u':
                {
                    switch (length) {
                        case 0:
                        {
                            unsigned int integer = va_arg(list, unsigned int);
                            __uint_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'H':
                        {
                            unsigned char integer = (unsigned char) va_arg(list, unsigned int);
                            __uint_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'h':
                        {
                            unsigned short int integer = va_arg(list, unsigned int);
                            __uint_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'l':
                        {
                            unsigned long integer = va_arg(list, unsigned long);
                            __uint_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'q':
                        {
                            unsigned long long integer = va_arg(list, unsigned long long);
                            __uint_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'j':
                        {
                            uintmax_t integer = va_arg(list, uintmax_t);
                            __uint_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'z':
                        {
                            size_t integer = va_arg(list, size_t);
                            __uint_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 't':
                        {
                            ptrdiff_t integer = va_arg(list, ptrdiff_t);
                            __uint_str((uintmax_t)integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        default:
                            break;
                    }
                    break;
                }

                case 'd':
                case 'i':
                {
                    switch (length) {
                        case 0:
                        {
                            int integer = va_arg(list, int);
                            __int_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'H':
                        {
                            signed char integer = (signed char) va_arg(list, int);
                            __int_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'h':
                        {
                            short int integer = va_arg(list, int);
                            __int_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'l':
                        {
                            long integer = va_arg(list, long);
                            __int_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'q':
                        {
                            long long integer = va_arg(list, long long);
                            __int_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'j':
                        {
                            intmax_t integer = va_arg(list, intmax_t);
                            __int_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 'z':
                        {
                            size_t integer = va_arg(list, size_t);
                            __int_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        case 't':
                        {
                            ptrdiff_t integer = va_arg(list, ptrdiff_t);
                            __int_str(integer, intStrBuffer, base, plusSign, spaceNoSign, lengthSpec, leftJustify, zeroPad);
                            OUTPUTSTRING(intStrBuffer);
                            break;
                        }
                        default:
                            break;
                    }
                    break;
                }

                case 'c':
                {
                    OUTPUTCHAR(va_arg(list, int));
                    break;
                }

                case 's':
                {
                    char *str_arg = va_arg(list, char*);
                    if (str_arg) OUTPUTSTRING(str_arg);
                    break;
                }

                case 'n':
                {
                    switch (length) {
                        case 'H':
                            *(va_arg(list, signed char*)) = chars;
                            break;
                        case 'h':
                            *(va_arg(list, short int*)) = chars;
                            break;

                        case 0: {
                            int* a = va_arg(list, int*);
                            *a = chars;
                            break;
                        }

                        case 'l':
                            *(va_arg(list, long*)) = chars;
                            break;
                        case 'q':
                            *(va_arg(list, long long*)) = chars;
                            break;
                        case 'j':
                            *(va_arg(list, intmax_t*)) = chars;
                            break;
                        case 'z':
                            *(va_arg(list, size_t*)) = chars;
                            break;
                        case 't':
                            *(va_arg(list, ptrdiff_t*)) = chars;
                            break;
                        default:
                            break;
                    }
                    break;
                }

                case 'a':
                case 'A':
                    //ACK! Hexadecimal floating points...
                    break;

                default:
                    break;
            }

            if (specifier == 'e') {
                displayString("e+", &chars);
            } else if (specifier == 'E') {
                displayString("E+", &chars);
            }

            if (specifier == 'e' || specifier == 'E') {
                __int_str(expo, intStrBuffer, 10, false, false, 2, false, true);
                OUTPUTSTRING(intStrBuffer);
            }

        } else {
            OUTPUTCHAR(format[i]);
        }
    }

    OUTPUTCHAR('\0');

    return chars;
}
