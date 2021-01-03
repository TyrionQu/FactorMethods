// ConnectEdonusMain.cpp : Defines the entry point for the console application.
//

#include <string>
#include <map>
#include <memory>
#include <iostream>
#include <iomanip>
#include <chrono>

#include <assert.h>
#include <gmp.h>
#include <unistd.h>
#include <variant>
#include <any>

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

void fermet_little_theorem_ex()
{
    mpz_t n, p, q, p_1, q_1, powm_p, powm_q, base, temp_p, temp_q, p_1Xq_1;
    mpz_init(n);
    mpz_init(p);
    mpz_init(q);
    mpz_init(p_1);
    mpz_init(q_1);
    mpz_init(temp_p);
    mpz_init(temp_q);
    mpz_init(p_1Xq_1);
    mpz_set_str(p, p_330, 10);
    mpz_set_str(q, q_330, 10);
    mpz_sub_ui(p_1, p, 1);
    mpz_sub_ui(q_1, q, 1);
    mpz_init(powm_p);
    mpz_init(powm_q);
    mpz_init(base);
    mpz_set_ui(base, 2);
    mpz_mul(n, p, q);
    mpz_mul(p_1Xq_1, p_1, q_1);
    cout << endl << "n is:";
    mpz_out_str(stdout, 10, n);
    cout << endl;

    cout << "2^(p-1) mod p is :";
    mpz_powm(powm_p, base, p_1, p);
    mpz_out_str(stdout, 10, powm_p);
    cout << endl << "2^(q-1) mod q is :";
    mpz_powm(powm_q, base, q_1, q);
    mpz_out_str(stdout, 10, powm_q);
    cout << endl;
    cout << "2^(p-1) mod n is :";
    mpz_powm(powm_p, base, p_1, n);
    mpz_out_str(stdout, 10, powm_p);
    mpz_sub_ui(temp_p, powm_p, 1);
    cout << endl << "(2^(p-1))^(q-1) is :";

    mpz_powm(powm_q, powm_p, q_1, n);
    mpz_out_str(stdout, 10, powm_q);
    cout << endl << "2^(q-1) mod n is :";

    mpz_powm(powm_q, base, q_1, n);
    mpz_out_str(stdout, 10, powm_q);
    mpz_sub_ui(temp_q, powm_q, 1);
    cout << endl << "(2^(q-1))^(p-1) is :";
    mpz_powm(powm_p, powm_q, p_1, n);
    mpz_out_str(stdout, 10, powm_p);
    cout << endl << "2^(p-1)(q-1) mod n is :";
    mpz_powm(powm_p, base, p_1Xq_1, n);
    mpz_out_str(stdout, 10, powm_p);
    mpz_mul(temp_p, temp_p, temp_q);
    mpz_mod(temp_p, temp_p, n);
    cout << endl << "(2^(p-1) - 1)(2^(q-1) - 1) mod n is :";
    mpz_out_str(stdout, 10, temp_p);
    cout << endl << " ... so 2^(p+q-2) + 1 = 2^(n-1) + 1 = 2^(p-1) + 2^(q-1) mod n" << endl;
    mpz_powm(temp_q, base, q_1, n);
    mpz_powm(temp_p, base, p_1, n);
    mpz_add(temp_p, temp_p, temp_q);
    mpz_mod(temp_p, temp_p, n);
    mpz_out_str(stdout, 10, temp_p);
    cout << endl;
    mpz_sub_ui(temp_p, n, 1);
    mpz_powm(powm_p, base, temp_p, n); 
    mpz_add_ui(temp_p, powm_p, 1);
    mpz_out_str(stdout, 10, temp_p);
    cout << endl;
    mpz_add(temp_p, p, q);
    mpz_sub_ui(temp_p, temp_p, 2);
    mpz_powm(powm_p, base, temp_p, n); 
    mpz_add_ui(temp_p, powm_p, 1);
    mpz_out_str(stdout, 10, temp_p);
    cout << endl;
    cout << "Verified!!! so 2^n + 2 = 2^p + 2^q mod n AND 2 * 2^n = 2^p * 2^q mod n" << endl;
    mpz_powm(temp_q, base, q, n);
    mpz_powm(temp_p, base, p, n);
    mpz_add(temp_p, temp_p, temp_q);
    mpz_mod(temp_p, temp_p, n);
    mpz_out_str(stdout, 10, temp_p);
    cout << endl;
    mpz_powm(powm_p, base, n, n); 
    mpz_add_ui(temp_p, powm_p, 2);
    mpz_out_str(stdout, 10, temp_p);
    cout << endl;

    mpz_clear(n);
    mpz_clear(p);
    mpz_clear(q);
    mpz_clear(p_1);
    mpz_clear(q_1);
    mpz_clear(temp_p);
    mpz_clear(temp_q);
    mpz_clear(p_1Xq_1);
    mpz_clear(powm_p);
    mpz_clear(powm_q);
    mpz_clear(base);
}

bool find_prime_factors(const mpz_t& source, mpz_t& left)
{
    bool bCompleted = false;
    mpz_t result;
    mpz_t temp;
    mpz_init(result);
    mpz_init(temp);
    mpz_set(result, source);
    int i = 0;
    int nCount = 0;
    int last_prime = primes[primes_size-1];
    cout << endl;
    while (mpz_cmp_ui(source, 1) != 0 && i < primes_size )
    {
        int cur_prime;
        if (i < primes_size)
        {
            cur_prime = primes[i];
        }
        else
        {
            cur_prime = last_prime + 2;
        }
        mpz_mod_ui(temp, result, cur_prime);
        if (mpz_cmp_ui(temp, 0) == 0)
        {
            mpz_div_ui(result, result, cur_prime);
            nCount++;
        }
        else
        {
            if (nCount != 0)
            {
                if (nCount == 1)
                    cout << cur_prime << " * ";
                else
                    cout << cur_prime << "^" << nCount << " * ";
            }
            i ++;
            nCount = 0;
        }
        last_prime = cur_prime;
    }
    mpz_out_str(stdout, 10, result);
    cout << endl << endl;
    mpz_clear(result);
    mpz_clear(temp);
    return bCompleted;
}

const int max_remainder = 1024*1024;
typedef std::shared_ptr<mpz_t[max_remainder]> mpz_shared_ptr;

class remainders {
public:
    mpz_shared_ptr remainder;
    int left = max_remainder;

    remainders() {
        mpz_shared_ptr sp(new mpz_t[max_remainder], [](mpz_t *p) { delete[] p; });
        remainder = sp;
        for (int i=0; i<max_remainder; i++)
            mpz_init(remainder[i]);
    }

    ~remainders() {
        for (int i=0; i<max_remainder; i++)
            mpz_clear(remainder[i]);
    }
};

bool find_loop(const mpz_t source)
{
    bool bFound = false;
    mpz_t temp, base, expr, temp_sqrt, temp_mul;
    remainders a;
    const auto p1 = std::chrono::steady_clock::now();

    mpz_init(temp);
    mpz_init(base);
    mpz_init(expr);
    mpz_init(temp_sqrt);
    mpz_init(temp_mul);
    mpz_set_ui(base, 2);
    mpz_set_ui(expr, 2);

    while (a.left>0 && !bFound) {
        mpz_powm(temp, base, expr, source);

        mpz_sqrt(temp_sqrt, temp);
        mpz_mul(temp_mul, temp_sqrt, temp_sqrt);
        if (mpz_cmp(temp_mul, temp) == 0 && mpz_cmp(temp_sqrt, base) != 0) {
            bFound = true;
            cout << endl << endl << "Fount complete sqrt!" << endl << endl;
            break;
        }

        for (int i = 0; i < (max_remainder - a.left); i++) {
            if (mpz_cmp(a.remainder[i], temp) == 0) {
                bFound = true;
                break;
            }
        }
        if (bFound == false) {
            mpz_set(a.remainder[max_remainder-a.left], temp);
      //      cout << max_remainder-a.left << " bits " << mpz_sizeinbase(temp, 2) << ": ";
            mpz_out_str(stdout, 10, temp);
            cout << endl;
            auto p2 = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed_seconds = p2 - p1;
     //       cout << " spend about: " << elapsed_seconds.count() << endl;
            mpz_set(base, temp);
            a.left--;
        }
    }
    if (bFound) {
        cout << "FOUND@@@:";
        mpz_out_str(stdout, 10, temp);
        cout << endl;
    }
    cout << endl << "Press <Enter> to continue...";
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
    cin.get();
    mpz_clear(temp);
    mpz_clear(base);
    mpz_clear(expr);
    mpz_clear(temp_sqrt);
    return bFound;
}

const int nDiv = 4;
void verify_result(const mpz_t p, const mpz_t q)
{
    mpz_t n, result, temp, base;
    mpz_init(n);
    mpz_init(result);
    mpz_init(temp);
    mpz_init(base);

    mpz_set_ui(base, 2);
    mpz_mul(n, p, q);

    mpz_add(temp, p, q);
    cout << endl << endl << "p+q = ";
    mpz_out_str(stdout, 10, temp);
    mpz_sub_ui(temp, n, 1);
    cout << endl << "n-1 = ";
    mpz_out_str(stdout, 10, temp);
    find_prime_factors(temp, result);
    mpz_add_ui(temp, n, 1);
    cout << endl << "n+1 = ";
    mpz_out_str(stdout, 10, temp);
    find_prime_factors(temp, result);
    mpz_sub(temp, n, p);
    mpz_sub(temp, temp, q);
    mpz_add_ui(temp, temp, 1);
    cout << endl << "n+1-p-q = ";
    mpz_out_str(stdout, 10, temp);
    find_prime_factors(temp, result);
    mpz_div_ui(temp, temp, nDiv);
    mpz_powm(temp, base, temp, n);
    cout << endl << "2^((n+1-p-q)/" << nDiv << ") = ";
    mpz_out_str(stdout, 10, temp);
    mpz_add_ui(temp, n, 1); 
    cout << endl;
    mpz_powm(temp, base, temp, n);
    cout << endl << "2^(n+1) mod n = ";
    mpz_out_str(stdout, 10, temp);
    cout << endl;
    mpz_add(temp, p, q);
    mpz_powm(temp, base, temp, n);
    cout << "2^(p+q) mod n = ";
    mpz_out_str(stdout, 10, temp);
    mpz_add_ui(temp, n, 1);
    mpz_div_ui(temp, temp, 2);
    mpz_powm(temp, base, temp, n);
    cout << endl << "2^((n+1)/2) mod n = ";
    mpz_out_str(stdout, 10, temp);
    cout << endl;
    mpz_add(temp, p, q);
    mpz_div_ui(temp, temp, 2);
    mpz_powm(temp, base, temp, n);
    cout << "2^((p+q)/2) mod n = ";
    mpz_out_str(stdout, 10, temp);

    mpz_add_ui(temp, n, 1);
    mpz_mul(temp, temp, temp);
    mpz_powm(temp, base, temp, n);
    cout << endl << "2^((n+1)(n+1)) mod n = ";
    mpz_out_str(stdout, 10, temp);
    cout << endl;
    mpz_add(temp, p, q);
    mpz_mul(temp, temp, temp);
    mpz_powm(temp, base, temp, n);
    cout << "2^((p+q)(p+q)) mod n = ";
    mpz_out_str(stdout, 10, temp);

    mpz_mul(temp, n, n);
    mpz_add_ui(temp, temp, 1);
    mpz_powm(temp, base, temp, n);
    cout << endl << "2^(n*n+1)   mod n = ";
    mpz_out_str(stdout, 10, temp);
    cout << endl;
    mpz_mul(temp, p, p);
    mpz_mul(result, q, q);
    mpz_add(temp, temp, result);
    mpz_powm(temp, base, temp, n);
    cout << "2^(p*p+q*q) mod n = ";
    mpz_out_str(stdout, 10, temp);

    mpz_sub_ui(temp, n, 1);
    mpz_mul(temp, temp, temp);
    mpz_powm(temp, base, temp, n);
    cout << endl << "2^((n-1)(n-1)) mod n = ";
    mpz_out_str(stdout, 10, temp);
    cout << endl;
    mpz_sub(temp, p, q);
    mpz_mul(temp, temp, temp);
    mpz_powm(temp, base, temp, n);
    cout << "2^((p-q)(p-q)) mod n = ";
    mpz_out_str(stdout, 10, temp);

    mpz_sub_ui(temp, n, 1);
    mpz_powm(temp, base, temp, n);
    cout << endl << "2^(n-1) mod n = ";
    mpz_out_str(stdout, 10, temp);
    cout << endl;
    mpz_sub(temp, q, p);
    mpz_powm(temp, base, temp, n);
    cout << "2^(q-p) mod n = ";
    mpz_out_str(stdout, 10, temp);
    cout << endl;

    mpz_clear(n);
    mpz_clear(result);
    mpz_clear(temp);
    mpz_clear(base);
}

int main(int argc, char *argv[])
{
    mpz_t n, result, remains, p, q;
    int flag;

    fermet_little_theorem_ex();
    // Initialize the number
    mpz_init(n);
    mpz_init(p);
    mpz_init(q);
    mpz_set_ui(n, 0);
    mpz_init(result);
    mpz_set_ui(result, 0);
    mpz_init(remains);
    mpz_set_ui(remains, 0);

    // Parse the input string as a base 10 number
    flag = mpz_set_str(n, rsa_330, 10);
    assert (flag == 0);
    flag = mpz_set_str(p, p_330, 10);
    assert (flag == 0);
    flag = mpz_set_str(q, q_330, 10);
    assert (flag == 0);

    // display n
    cout << "bits " << mpz_sizeinbase(n, 2) << " n = ";
    mpz_out_str(stdout, 10, n);
    verify_result(p, q);
    cout << __LINE__ << ": ok"<< endl;
    cout << endl << "Press <Enter> to continue...";
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
    flag = mpz_set_str(n, rsa_2048, 10);
//    find_loop(n);
//    find_sqrt(n);

    //mpz_div(result, n, g_nBase);
//    mpz_fdiv_qr_ui(result, remains, n, g_nBase);

    cout << __LINE__ << ": ok"<< endl;
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
    cout << endl << "base = " << g_nBase << " y = " << endl;
    mpz_out_str(stdout, 16, y);
    cout << endl;
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
