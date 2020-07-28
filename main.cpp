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
    return i--;
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
    cout << endl << " n / " << g_nBase << " = ";
    mpz_out_str(stdout, 10, result);
    cout << "    remains is: ";
    mpz_out_str(stdout, 10, remains);

    int ii = get_index(n, g_nBase);
	cout << endl << ii << " press enter to continue ..." << endl;
	cin.ignore();
	return 0;
}
