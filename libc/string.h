#ifndef STRINGS_H
#define STRINGS_H

#include <libc/stdint>


void int_to_ascii(const int n, char str[]);
void hex_to_ascii(const int n, char str[]);
void h2c(const int n, char str[]);
void reverse(char s[]);
int strlen(const char s[]);
void backspace(char s[]);
void append(char s[], const char n);
int strcmp(const char s1[],const  char s2[]);
void cut(char s[], const int from, const int to);
void clear(char s[],int length);
char* cutr(char s[], const int from, const int to);
void split(const char s[], char buf[], const char token, const int count);  // count - по-счёту, не idx
int splitidx(const char s[], const char token, const int count);         // получение начала токена по счёту count

int strncmp(const char* str1, const char* str2, const uint32_t n);

//true if str has 1+ char from target
char subsymbolac(const char str[], const char target);
char subsymbolaa(const char str[], const char target[]);
char subsymbolca(const char str, const char target[]);

char toSmall(const char c);

char* strncpy(char* destination, const char* source, const uint32_t num);

#endif
