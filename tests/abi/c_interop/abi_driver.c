/* Independent GCC-generated callers check the DMM callee's ABI, rather than
 * relying solely on agreement between the DMM caller and callee. */
#include <stdio.h>

extern int abi_ints(int,int,int,int,int,int,int,int);
extern double abi_floats(double,double,double,double,double,double,double,double,double);
extern double abi_mixed(int,double,int,double,int,double,int,double,int,double,int,double,int,double,int,double,double,double,double);
extern float abi_float(float,float,float,float,float);
extern unsigned char abi_strings(const char *,const char *);
extern int abi_zero(void);
extern int abi_one(int);
extern int abi_six(int,int,int,int,int,int);
extern int abi_seven(int,int,int,int,int,int,int);
extern int abi_eight(int,int,int,int,int,int,int,int);
extern double abi_double_one(double);
extern double abi_double_eight(double,double,double,double,double,double,double,double);
extern double abi_double_nine(double,double,double,double,double,double,double,double,double);

int main(void) {
    if (abi_zero() != 42) return 10;
    if (abi_one(41) != 42) return 11;
    if (abi_six(1,2,3,4,5,6) != 21) return 12;
    if (abi_seven(1,2,3,4,5,6,7) != 28) return 13;
    if (abi_eight(1,2,3,4,5,6,7,8) != 36) return 14;
    if (abi_double_one(1.25) != 1.25) return 15;
    if (abi_double_eight(1,2,3,4,5,6,7,8) != 36.0) return 16;
    if (abi_double_nine(1,2,3,4,5,6,7,8,9) != 45.0) return 17;
    if (abi_ints(1,2,3,4,5,6,7,8) != 204) return 1;
    if (abi_floats(1,2,3,4,5,6,7,8,9) != 285.0) return 2;
    if (abi_mixed(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19) != 2470.0) return 3;
    if (abi_float(1.25f,2.5f,3.75f,4,5) != 16.5f) return 4;
    char first[] = "same text", second[] = "same text";
    if (!abi_strings(first, second) || abi_strings(first, "different")) return 5;
    puts("GCC ABI interop OK");
    return 0;
}
/* Category: ABI/C interoperability. */
