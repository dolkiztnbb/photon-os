#include "math.h"


EXTENDED_KERNEL float deg2rad(float deg){
	return M_PI*deg/180.0f;
}

EXTENDED_KERNEL double cosf(double x) {
	while (x > M_PI)  x -= 2*M_PI;
	while (x < -M_PI) x += 2*M_PI;

	double x2 = x * x;
	return 1
	       - x2 / 2
	       + x2*x2 / 24
	       - x2*x2*x2 / 720;
}

EXTENDED_KERNEL double sinf(double x) {
	while (x > M_PI)  x -= 2*M_PI;
	while (x < -M_PI) x += 2*M_PI;

	double x2 = x * x;
	return x
	       - x*x2 / 6
	       + x*x2*x2 / 120
	       - x*x2*x2*x2 / 5040;
}

EXTENDED_KERNEL unsigned long long pow(const int a1, const int b){
    int a=1;
    for(int i=1;i<=b;i++){
	    a*=a1;
    }
    return a;
}

EXTENDED_KERNEL unsigned long long factorial(const int n) {
    unsigned long long result = 1;
    for (int i = 1; i <= n; ++i) {
        result *= i;
    }
    return result;
}

EXTENDED_KERNEL int min(const int a, const int b){
    return a < b ? a : b;
}

EXTENDED_KERNEL int max(const int a, const int b){
    return a > b ? a : b;
}

EXTENDED_KERNEL int mathf(const int a, const int mi, const int ma){
    return a < mi ? mi : (a > ma? ma : a);
}

EXTENDED_KERNEL char mathff(const int a, const int mi, const int ma){
    return a >= mi && a <= ma;
}

EXTENDED_KERNEL double tanf(const float x){
	return sinf(x)/cosf(x);
}
