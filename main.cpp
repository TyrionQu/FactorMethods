// ConnectEdonusMain.cpp : Defines the entry point for the console application.
//

#include <string>
#include <iostream>
#include <iomanip>

#include <assert.h>
#include <gmp.h>

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
    cout << "n = ";
    mpz_out_str(stdout, 10, n);

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
        cout << setw(5) <<coefficient[i];
        if (i != 0 && i != 1)
            cout << " x^" << i << " + ";
        else if (i == 1)
            cout << " x" << " + ";
        if (i % 8 == 0)
            cout << endl;
    }
    if (mpz_cmp(y, n) == 0) {
        cout << endl << "Result is right!";
    }
    cout << endl << "y = ";
    mpz_out_str(stdout, 10, y);
    mpz_clear(x);
    mpz_clear(y);
    mpz_clear(n);
    mpz_clear(result);
    mpz_clear(remains);
	cout << endl << ii << " press enter to continue ..." << endl;
	cin.ignore();
    delete [] coefficient;
	return 0;
}
