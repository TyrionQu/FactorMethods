// ConnectEdonusMain.cpp : Defines the entry point for the console application.
//

#include <string>
#include <iostream>
#include <iomanip>

#include <assert.h>
#include <gmp.h>
#include <unistd.h>

#include "constant.h"

using namespace std;

int get_index(mpz_t n, unsigned int nBase)
{
    int i = 1;
    mpz_t r, base;

    mpz_init(r);
    mpz_init(base);
    mpz_set_ui(r, nBase);
    mpz_set_ui(base, nBase);
    while (mpz_cmp(r, n) < 0) {
        mpz_mul(r, r, base);
        i++;
    }
    mpz_clear(r);
    mpz_clear(base);
    return --i;
}

int find_coefficient(mpz_t n, unsigned int nBase, unsigned short *coefficient, unsigned short nSize)
{
    int i = nSize - 1;
    mpz_t y, base;
    mpz_init(y);
    mpz_set(y, n);
    mpz_init(base);
    mpz_set_ui(base, nBase);

    for (; i > 0; i--) {
        mpz_t tmp1, tmp2;
        mpz_init(tmp1);
        mpz_init(tmp2);
        mpz_set_ui(tmp1, 0);
        mpz_set_ui(tmp2, 0);

        mpz_pow_ui(tmp1, base, i);
        int nMax = nBase;
        int nMin = 0;
        while (nMin + 1 < nMax) {
            int nTmp = (nMax + nMin) / 2;
            mpz_mul_ui(tmp2, tmp1, nTmp);
            int nCmp = mpz_cmp(tmp2, y);
            if (nCmp < 0) {
                nMin = nTmp;
            }
            else if (nCmp == 0)
            {
                break;
            }
            else
            {
                nMax = nTmp;
            }

        }
        mpz_mul_ui(tmp2, tmp1, nMin);
        mpz_sub(y, y, tmp2);
        coefficient[i] = nMin;
        mpz_clear(tmp1);
        mpz_clear(tmp2);
    }

    int a0 = mpz_get_ui(y);

    if (a0 < nBase)
        coefficient[0] = (unsigned short)a0;
    return 0;
}

int find_sqrt(const mpz_t n)
{
    mpz_t sqrt_n, next, expx;
    mpz_t diff1, diff2;
    mpz_init(expx);
    mpz_set_ui(expx, 0);
    mpz_init(sqrt_n);
    mpz_set_ui(sqrt_n, 0);
    mpz_init(diff1);
    mpz_set_ui(diff1, 0);
    mpz_init(diff2);
    mpz_set_ui(diff2, 0);
    mpz_init(next);

    mpz_sqrt(sqrt_n, n);
    cout << endl << "sqrt(n) " << mpz_sizeinbase(sqrt_n, 2) << " bits: ";
    mpz_out_str(stdout, 10, sqrt_n);
    mpz_set(next, sqrt_n);
    mpz_mul(diff1, sqrt_n, sqrt_n);
    mpz_sub(diff1, n, diff1);
    if (mpz_cmp_ui(diff1, 0) == 0) {
        cout << endl << "Found@@" << endl;
        return 0;
    }
    // diff2 = 2 * next + 1
    mpz_mul_ui(diff2, next, 2);
    mpz_add_ui(diff2, diff2, 1);

    // expx = diff2
    mpz_sub(expx, diff2, diff1);

    int64_t last = 0xffffffffff;
    mpz_t nextX2_1, x;
    mpz_t xxx, lastx;
    mpz_init(nextX2_1);
    mpz_init(x);
    mpz_init(xxx);
    mpz_init(lastx);
    mpz_set_ui(lastx, 0);
    for (int64_t i = 0; i <= last; i++) {
        // next = 2 * next + 1
        mpz_add_ui(next, next, 1);
        mpz_mul_ui(nextX2_1, next, 2);
        mpz_add_ui(nextX2_1, nextX2_1, 1);
        // expx = expx + 2 * next + 1
        mpz_add(expx, expx, nextX2_1);
        mpz_sqrt(x, expx);
        mpz_mul(xxx, x, x);
        mpz_sub(nextX2_1, expx, xxx);
        if (i % 0xfffff == 1 ) {
            cout << endl << setw(10) << right << i << " x " << mpz_sizeinbase(x, 2)  << " bits, diff ";
//            mpz_out_str(stdout, 10, x);
            mpz_sub(xxx, x, lastx);
            cout << mpz_sizeinbase(xxx, 2) << " bits, diff = ";
            mpz_out_str(stdout, 10, xxx);
        }

        if (mpz_cmp_ui(nextX2_1, 0) == 0) {
            cout << endl << "found!!!" << endl;
            mpz_out_str(stdout, 10, x);
            break;
        }
//        cout << endl << "diff = ";
//        mpz_out_str(stdout, 10, nextX2_1);
        mpz_set(lastx, x);
    }
    mpz_clear(nextX2_1);
    mpz_clear(xxx);
    mpz_clear(x);
    return 0;
}

int main(int argc, char *argv[])
{
    mpz_t n, result, remains;
    int flag;

    // Initialize the number
    mpz_init(n);
    mpz_set_ui(n, 0);
    mpz_init(result);
    mpz_set_ui(result, 0);
    mpz_init(remains);
    mpz_set_ui(remains, 0);

    // Parse the input string as a base 10 number
    flag = mpz_set_str(n, rsa_2048, 10);
    assert (flag == 0);

    // display n
    cout << "bits " << mpz_sizeinbase(n, 2) << " n = ";
    mpz_out_str(stdout, 10, n);

    find_sqrt(n);
    //mpz_div(result, n, g_nBase);
    mpz_fdiv_qr_ui(result, remains, n, g_nBase);

    int ii = get_index(n, g_nBase) + 1;
    unsigned short *coefficient = new unsigned short[ii];
    find_coefficient(n, g_nBase, coefficient, ii);

    cout << endl;
    mpz_t y, x;
    mpz_init(x);
    mpz_init(y);
    mpz_set_ui(x, g_nBase);
    mpz_set_ui(y, 0);
    for (int i = ii - 1; i >= 0; i --) {
        mpz_t tmp;
        mpz_init(tmp);
        mpz_set_ui(tmp, 0);
        mpz_mul(y, y, x);
        mpz_add_ui(y, y, coefficient[i]);
        mpz_clear(tmp);
        cout << setw(5) << right <<coefficient[i];
        if (i != 0 && i != 1)
            cout << " * x^" << setw(3) << left << i << " + ";
        else if (i == 1)
            cout << setw(8) << left << " * x" << " + ";
        if (i % 8 == 0)
            cout << endl;
    }
    if (mpz_cmp(y, n) == 0) {
        cout << endl << "Result is right!";
    }
    cout << endl << "base = " << g_nBase << " y = ";
    mpz_out_str(stdout, 16, y);
    mpz_clear(x);
    mpz_clear(y);
    mpz_clear(n);
    mpz_clear(result);
    mpz_clear(remains);
	cout << endl << ii << " press enter to continue ..." << endl;
//	cin.ignore();
    delete [] coefficient;
	return 0;
}
