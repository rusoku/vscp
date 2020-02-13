// FILE: vscphelper.cpp
//
// This file is part of the VSCP (https://www.vscp.org)
//
// The MIT License (MIT)
//
// Copyright © 2000-2020 Ake Hedman, Grodans Paradis AB
// <info@grodansparadis.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include <limits.h>
#include <math.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#define access _access_s
#endif

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <locale>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <expat.h>
#include <json.hpp> // Needs C++11  -std=c++11

#include <canal_macro.h>
#include <crc.h>
#include <crc8.h>
#include <fastpbkdf2.h>
#include <vscp_aes.h>
#include <vscpbase64.h>
#include <vscpmd5.h>

#include <guid.h>
#include <mdf.h>
#include <vscp.h>
#include <vscphelper.h>

#ifdef WIN32
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#else
#endif

#define UNUSED(expr)                                                           \
    do {                                                                       \
        (void)(expr);                                                          \
    } while (0)

#define vsnprintf_impl vsnprintf

#define XML_BUFF_SIZE 0xffff

using namespace std;

// https://github.com/nlohmann/json
using json = nlohmann::json;

#define Swap8Bytes(val)                                                        \
    ((((val) >> 56) & 0x00000000000000FF) |                                    \
     (((val) >> 40) & 0x000000000000FF00) |                                    \
     (((val) >> 24) & 0x0000000000FF0000) |                                    \
     (((val) >> 8) & 0x00000000FF000000) |                                     \
     (((val) << 8) & 0x000000FF00000000) |                                     \
     (((val) << 24) & 0x0000FF0000000000) |                                    \
     (((val) << 40) & 0x00FF000000000000) |                                    \
     (((val) << 56) & 0xFF00000000000000))

// Check windows
#if _WIN32 || _WIN64
#if _WIN64
#define ENVIRONMENT64
#else
#define ENVIRONMENT32
#endif
#endif

// Check GCC
#if __GNUC__
#if __x86_64__ || __ppc64__
#define ENVIRONMENT64
#else
#define ENVIRONMENT32
#endif
#endif

// ***************************************************************************
//                              General Helpers
// ***************************************************************************

////////////////////////////////////////////////////////////////////////////////
// vscp_readStringValue

int32_t
vscp_readStringValue(const std::string& strval)
{
    int32_t val     = 0;
    std::string str = strval;
    vscp_makeLower(str);
    vscp_trim(str);

    try {
        std::size_t pos;
        if (string::npos != (pos = str.find("0x"))) {
            str = str.substr(2);
            val = std::stoul(str, &pos, 16);
        } else if (string::npos != (pos = str.find("0o"))) {
            str = str.substr(2);
            val = std::stoul(str, &pos, 8);
        } else if (string::npos != (pos = str.find("0b"))) {
            str = str.substr(2);
            val = std::stoul(str, &pos, 2);
        } else {
            val = std::stoul(str);
        }
    } catch (std::invalid_argument&) {
        val = 0;
    } catch (std::out_of_range&) {
        val = 0;
    }

    return val;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_is64Bit
//

int
vscp_is64Bit()
{
#ifdef ENVIRONMENT64
    return -1;
#else
    return 0;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// vscp_is32Bit
//

int
vscp_is32Bit()
{
#ifdef ENVIRONMENT32
    return -1;
#else
    return 0;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// vscp_isLittleEndian
//

int
vscp_isLittleEndian(void)
{
    int x = 1;
    return (*(char*)&x);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_isBigEndian
//
//

int
vscp_isBigEndian(void)
{
    return !vscp_isLittleEndian();
}

///////////////////////////////////////////////////////////////////////////////
// vscp_mem_usage
//
// https://gist.github.com/thirdwing/da4621eb163a886a03c5

#ifndef WIN32
void
vscp_mem_usage(double& vm_usage, double& resident_set)
{
    vm_usage     = 0.0;
    resident_set = 0.0;

    // the two fields we want
    unsigned long vsize;
    long rss;

    {
        std::string ignore;
        std::ifstream ifs("/proc/self/stat",
                          std::ios_base::in); // get info from proc directory
        ifs >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >>
          ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >>
          ignore >> ignore >> ignore >> ignore >> ignore >> ignore >> ignore >>
          ignore >> ignore >> vsize >> rss;
        ifs.close();
    }
    long page_size_kb = sysconf(_SC_PAGE_SIZE) /
                        1024; // in case x86-64 is configured to use 2MB pages
    vm_usage     = vsize / 1024.0;
    resident_set = rss * page_size_kb;
}
#endif

///////////////////////////////////////////////////////////////////////////////
// vscp_sem_wait
//

int
vscp_sem_wait(sem_t* sem, uint32_t waitms)
{
    uint64_t ns;
    struct timespec ts;

    // Wait time must be less than four seconds
    if (waitms >= 4000)
        return -1;

    if (-1 == clock_gettime(CLOCK_REALTIME, &ts)) {
        return -1;
    }

    ns = ts.tv_nsec + (waitms * 1000000);
    ts.tv_sec += (ns / 1000000000);
    ts.tv_nsec = ns % 1000000000;

    return sem_timedwait(sem, &ts);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_almostEqualRelativeFloat
//

bool
vscp_almostEqualRelativeFloat(float A, float B, float maxRelDiff)
{
    // Calculate the difference.
    float diff = fabs(A - B);
    A          = fabs(A);
    B          = fabs(B);

    // Find the largest
    float largest = (B > A) ? B : A;

    if (diff <= (largest * maxRelDiff)) {
        return true;
    }

    return false;
}

union Float_t
{
    Float_t(float num = 0.0f)
      : f(num)
    {}
    // Portable extraction of components.
    bool Negative() const { return i < 0; }
    int32_t RawMantissa() const { return i & ((1 << 23) - 1); }
    int32_t RawExponent() const { return (i >> 23) & 0xFF; }

    int32_t i;
    float f;
#ifdef _DEBUG
    struct
    { // Bitfields for exploration. Do not use in production code.
        uint32_t mantissa : 23;
        uint32_t exponent : 8;
        uint32_t sign : 1;
    } parts;
#endif
};

///////////////////////////////////////////////////////////////////////////////
// vscp_almostEqualUlpsAndAbs
//

bool
vscp_almostEqualUlpsAndAbsFloat(float A,
                                float B,
                                float maxDiff,
                                int maxUlpsDiff)
{
    // Check if the numbers are really close -- needed
    // when comparing numbers near zero.
    float absDiff = fabs(A - B);
    if (absDiff <= maxDiff) {
        return true;
    }

    Float_t uA(A);
    Float_t uB(B);

    // Different signs means they do not match.
    if (uA.Negative() != uB.Negative()) {
        return false;
    }

    // Find the difference in ULPs.
    int ulpsDiff = abs(uA.i - uB.i);
    if (ulpsDiff <= maxUlpsDiff) {
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_almostEqualRelativeAndAbs
//

bool
vscp_almostEqualRelativeAndAbsFloat(float A,
                                    float B,
                                    float maxDiff,
                                    float maxRelDiff)
{
    // Check if the numbers are really close -- needed
    // when comparing numbers near zero.
    float diff = fabs(A - B);
    if (diff <= maxDiff) {
        return true;
    }

    A             = fabs(A);
    B             = fabs(B);
    float largest = (B > A) ? B : A;

    if (diff <= (largest * maxRelDiff)) {
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_almostEqualRelativeDouble
//

bool
vscp_almostEqualRelativeDouble(double A, double B, double maxRelDiff)
{
    // Calculate the difference.
    double diff = fabs(A - B);
    A           = fabs(A);
    B           = fabs(B);

    // Find the largest
    double largest = (B > A) ? B : A;

    if (diff <= (largest * maxRelDiff)) {
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_lowercase
//

int
vscp_lowercase(const char* s)
{
    return tolower(*(const unsigned char*)s);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_strcasecmp
//

int
vscp_strcasecmp(const char* s1, const char* s2)
{
    int diff;

    do {
        diff = vscp_lowercase(s1++) - vscp_lowercase(s2++);
    } while (diff == 0 && s1[-1] != '\0');

    return diff;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_strncasecmp
//

int
vscp_strncasecmp(const char* s1, const char* s2, size_t len)
{
    int diff = 0;

    if (len > 0) {
        do {
            diff = vscp_lowercase(s1++) - vscp_lowercase(s2++);
        } while (diff == 0 && s1[-1] != '\0' && --len > 0);
    }

    return diff;
}

// ------- civet

////////////////////////////////////////////////////////////////////////////////
// vscp_strlcpy
//

void
vscp_strlcpy(char* dst, const char* src, size_t n)
{
    for (; *src != '\0' && n > 1; n--) {
        *dst++ = *src++;
    }

    *dst = '\0';
}

////////////////////////////////////////////////////////////////////////////////
// vscp_strndup
//

char*
vscp_strndup(const char* ptr, size_t len)
{
    char* p;

    if ((p = (char*)malloc(len + 1)) != NULL) {
        vscp_strlcpy(p, ptr, len + 1);
    }

    return p;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_strdup
//

char*
vscp_strdup(const char* str)
{
    return vscp_strndup(str, strlen(str));
}

////////////////////////////////////////////////////////////////////////////////
// vscp_strcasestr
//

const char*
vscp_strcasestr(const char* big_str, const char* small_str)
{
    size_t i, big_len = strlen(big_str), small_len = strlen(small_str);

    if (big_len >= small_len) {

        for (i = 0; i <= (big_len - small_len); i++) {

            if (0 == vscp_strncasecmp(big_str + i, small_str, small_len)) {
                return big_str + i;
            }
        }
    }

    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_stristr
//

char*
vscp_stristr(char* str1, const char* str2)
{
    char* p1       = str1;
    const char* p2 = str2;
    char* r        = *p2 == 0 ? str1 : 0;

    while (*p1 != 0 && *p2 != 0) {
        if (tolower(*p1) == tolower(*p2)) {
            if (r == 0) {
                r = p1;
            }

            p2++;
        } else {
            p2 = str2;
            if (tolower(*p1) == tolower(*p2)) {
                r = p1;
                p2++;
            } else {
                r = 0;
            }
        }

        p1++;
    }

    return *p2 == 0 ? r : 0;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_trimWhiteSpace
//

char*
vscp_trimWhiteSpace(char* str)
{
    char* end;

    // Trim leading space
    while (isspace(*str))
        str++;

    if (0 == *str) { // All spaces?
        return str;
    }

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace(*end))
        end--;

    // Write new null terminator
    *(end + 1) = 0;

    return str;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_reverse
//

char*
vscp_reverse(const char* const s)
{
    if (NULL == s)
        return NULL;

    size_t i, len = strlen(s);
    char* r = (char*)malloc(len + 1);

    for (i = 0; i < len; ++i) {
        r[i] = s[len - i - 1];
    }

    r[len] = 0;
    return r;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_rstrstr
//

char*
vscp_rstrstr(const char* s1, const char* s2)
{
    size_t s1len = strlen(s1);
    size_t s2len = strlen(s2);
    char* s;

    if (s2len > s1len)
        return NULL;

    for (s = (char*)s1 + s1len - s2len; s >= s1; --s) {
        if (0 == strncmp(s, s2, s2len)) {
            return s;
        }
    }

    return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_str_format
//

std::string
vscp_str_format(const std::string& fmt_str, ...)
{
    int final_n,
      n = ((int)fmt_str.size()) *
          2; /* Reserve two times as much as the length of the fmt_str */
    std::unique_ptr<char[]> formatted;
    va_list ap;
    while (1) {
        formatted.reset(
          new char[n]); /* Wrap the plain char array into the unique_ptr */
        strcpy(&formatted[0], fmt_str.c_str());
        va_start(ap, fmt_str);
        final_n = vsnprintf(&formatted[0], n, fmt_str.c_str(), ap);
        va_end(ap);
        if (final_n < 0 || final_n >= n)
            n += abs(final_n - n + 1);
        else
            break;
    }

    return std::string(formatted.get());
}

///////////////////////////////////////////////////////////////////////////////
// vscp_startsWith
//

bool
vscp_startsWith(const std::string& origstr,
                const std::string& searchstr,
                std::string* rest)
{
    if (0 != origstr.find(searchstr)) {
        if (NULL != rest) {
            *rest = origstr;
        }
        return false;
    }

    if (NULL != rest) {
        *rest = origstr.substr(searchstr.length());
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_fileExists
//

bool
vscp_fileExists(const std::string& path)
{
    return access(path.c_str(), 0) == 0;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_dirExists
//

int
vscp_dirExists(const char* const path)
{
    struct stat info;

    int statRC = stat(path, &info);
    if (statRC != 0) {
        if (errno == ENOENT) {
            return 0;
        } // something along the path does not exist
        if (errno == ENOTDIR) {
            return 0;
        } // something in path prefix is not a dir
        return -1;
    }

    return (info.st_mode & S_IFDIR) ? 1 : 0;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getTimeString
//

bool
vscp_getTimeString(char* buf, size_t buf_len, time_t* t)
{
    // Check pointers
    if (NULL == buf)
        return false;
    if (NULL == t)
        return false;

#if !defined(REENTRANT_TIME)
	struct tm *tm;

	tm = ((t != NULL) ? gmtime(t) : NULL);
	if (tm != NULL) {
#else
	struct tm _tm;
	struct tm *tm = &_tm;

	if (t != NULL) {
		gmtime_r(t, tm);
#endif
		strftime(buf, buf_len, "%a, %d %b %Y %H:%M:%S GMT", tm);
	} else {
		vscp_strlcpy(buf, "Thu, 01 Jan 1970 00:00:00 GMT", buf_len);
		buf[buf_len - 1] = '\0';
	}

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getISOTimeString
//

bool
vscp_getISOTimeString(char* buf, size_t buf_len, time_t* t)
{
    // Check pointers
    if (NULL == buf)
        return false;
    if (NULL == t)
        return false;

    strftime(buf, buf_len, "%Y-%m-%dT%H:%M:%SZ", gmtime(t));

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_parseISOCombined
//

bool
vscp_parseISOCombined(struct tm* ptm, std::string& dt)
{
    size_t pos;
    std::string isodt = dt.c_str();

    // Check pointer
    if (NULL == ptm)
        return false;

    try {
        // year
        ptm->tm_year = stoi(isodt.c_str(), &pos) - 1900;
        pos++; // Move past '-'
        isodt = isodt.substr(pos);

        // month
        ptm->tm_mon = stoi(isodt.c_str(), &pos);
        pos++; // Move past '-'
        isodt = isodt.substr(pos);

        // day
        ptm->tm_mday = stoi(isodt.c_str(), &pos);
        pos++; // Move past 'T' or ' '
        isodt = isodt.substr(pos);

        // hour
        ptm->tm_hour = stoi(isodt.c_str(), &pos);
        pos++; // Move past ':'
        isodt = isodt.substr(pos);

        // minute
        ptm->tm_min = stoi(isodt.c_str(), &pos);
        pos++; // Move past ':'
        isodt = isodt.substr(pos);

        // second
        ptm->tm_sec = stoi(isodt.c_str(), &pos);
    } catch (...) {
        return false;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_toXMLEscape
//
// Escape "invalid" XML characters for insert in XML file.
// Note: Must be room in original buffer for result
//

bool
vscp_XML_Escape(char* dst, size_t dst_len, const char* src)
{
    /*const char escapeCharTbl[6]      = { '&', '\'', '\"', '>', '<', '\0' };
    const char *const escapeSeqTbl[] = {
        "&amp;", "&apos;", "&quot;", "&gt;", "&lt;",
    };


    unsigned int i, j, k;
    unsigned int nRef = 0;
    unsigned int nEscapeChars = strlen( escapeCharTbl );
    unsigned int str_len = strlen( src );
    int nShifts = 0;

    // ******  TODO TODP TODO TODO


    // Go through string
    for ( i = 0; i<str_len; i++ ) {

        // Go through escape chars
        for ( nRef = 0; nRef < nEscapeChars; nRef++ ) {

            // Check if char needing to be escaped on this pos
            if ( temp_str[ i ] == escapeChar[ nRef ] ) {

                if ( ( nShifts = strlen( escapeTable[ nRef ] ) - 1 ) > 0 ) {

                    memmove( temp_str + i + nShifts,
                                temp_str + i,
                                str_len - i + nShifts );

                    for ( j=i, k=0; j<=i+nShifts, k<=nShifts; j++,k++ ) {
                        temp_str[ j ] = escapeTable[ nRef ][ k ];
                    }

                    str_len += nShifts;
                }
            }
        }
    }

    dst[ str_len ] = '\0'; */

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_base64_std_decode
//

bool
vscp_base64_std_decode(std::string& str)
{
    size_t dest_len   = 0;
    size_t bufferSize = 2 * str.length();
    if (0 == str.length())
        return true; // Nothing to do if empty

    char* pbuf = new char[bufferSize];
    if (NULL == pbuf)
        return false;
    memset(pbuf, 0, bufferSize);

    vscp_base64_decode((const unsigned char*)((const char*)str.c_str()),
                       str.length(),
                       pbuf,
                       &dest_len);
    str = pbuf;
    delete[] pbuf;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_base64_std_encode
//

bool
vscp_base64_std_encode(std::string& str)
{
    size_t bufferSize = 2 * strlen((const char*)str.c_str());
    char* pbuf        = new char[bufferSize];
    if (NULL == pbuf)
        return false;
    memset(pbuf, 0, bufferSize);

    vscp_base64_encode((const unsigned char*)((const char*)str.c_str()),
                       strlen((const char*)str.c_str()),
                       pbuf);

    str = pbuf;
    delete[] pbuf;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// decodeBase64IfNeeded
//

bool
vscp_std_decodeBase64IfNeeded(const std::string& str, std::string& strResult)
{
    // If BASE64 encoded then decode
    strResult = str;
    vscp_trim(strResult);

    // A zero length string is accepted
    if (0 == strResult.length())
        return true;

    if (0 == strResult.find("BASE64:")) {
        strResult = strResult.substr(7, strResult.length() - 7);
        vscp_base64_std_decode(strResult);
    } else {
        strResult = str;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_convertToBase64
//

std::string
vscp_convertToBase64(std::string str)
{
    return (vscp_base64_std_encode(str) ? str : std::string(""));
}

////////////////////////////////////////////////////////////////////////////////
// isbyte
//

static int
isbyte(int n)
{
    return (n >= 0) && (n <= 255);
}

////////////////////////////////////////////////////////////////////////////////
// vscp_parse_ipv4_addr
//

int
vscp_parse_ipv4_addr(const char* addr, uint32_t* net, uint32_t* mask)
{
    int n, a, b, c, d, slash = 32, len = 0;

    if (((5 == sscanf(addr, "%d.%d.%d.%d/%d%n", &a, &b, &c, &d, &slash, &n)) ||
         (4 == sscanf(addr, "%d.%d.%d.%d%n", &a, &b, &c, &d, &n))) &&
        isbyte(a) && isbyte(b) && isbyte(c) && isbyte(d) && (slash >= 0) &&
        (slash < 33)) {

        len  = n;
        *net = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) |
               (uint32_t)d;
        *mask = slash ? (0xffffffffU << (32 - slash)) : 0;
    }

    return len;
}

// ***************************************************************************
//                              Data Coding Helpers
// ***************************************************************************

uint8_t
vscp_getMeasurementDataCoding(const vscpEvent* pEvent)
{
    uint8_t datacoding_byte = -1;

    if (NULL == pEvent)
        return -1;
    if (NULL == pEvent->pdata)
        return -1;
    if (pEvent->sizeData < 1)
        return -1;

    if (VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) {
        datacoding_byte = pEvent->pdata[0];
    } else if (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class) {
        if (pEvent->sizeData >= 16)
            datacoding_byte = pEvent->pdata[16];
    }

    return datacoding_byte;
}

//////////////////////////////////////////////////////////////////////////////
// getDataCodingBitArray
//

uint64_t
vscp_getDataCodingBitArray(const uint8_t* pCode, const uint8_t length)
{
    uint64_t bitArray = 0;

    if (NULL == pCode)
        return 0;
    if ((length > 7) || (length <= 1))
        return 0;

    for (int i = 0; i < length - 1; i++) {
        bitArray = bitArray << 8;
        bitArray |= *(pCode + 1 + i);
    }

    return bitArray;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_getDataCodingInteger
//

int64_t
vscp_getDataCodingInteger(const uint8_t* pCode, uint8_t length)
{
    int64_t value64 = 0;
    // uint8_t byteArray[8];

    if (NULL == pCode)
        return 0;
    if (length < 2)
        return 0;

    // Check if this is a negative number
    if ((*(pCode + 1)) & 0x80) {
        memset((uint8_t*)&value64, 0xff, 8);
    } else {
        memset((uint8_t*)&value64, 0, 8);
    }

    for (int i = 1; i < length; i++) {
        value64 = (value64 << 8) + pCode[i];
    }

    return value64;
}

//////////////////////////////////////////////////////////////////////////////
// getDataCodingNormalizedInteger
//

double
vscp_getDataCodingNormalizedInteger(const uint8_t* pCode, uint8_t length)
{
    uint8_t valarray[8];
    uint8_t normbyte;
    uint8_t decibyte;
#ifndef __BIG_ENDIAN__
    int64_t value64;
#endif
    double value   = 0;
    bool bNegative = false; // set for negative number

    // Check pointer
    if (NULL == pCode) {
        return 0;
    }

    // Check character count
    if ((length > 8) || (length < 2)) {
        return 0;
    }

    memset(valarray, 0, sizeof(valarray));
    normbyte = *pCode;
    UNUSED(normbyte);
    decibyte = *(pCode + 1);

    // Check if this is a negative number
    if ((*(pCode + 2)) & 0x80) {
        bNegative = true;
    }

    switch (length - 2) {

        case 1: // 8-bit
            memcpy((char*)&valarray, (pCode + 2), (length - 2));
            value = *((int8_t*)valarray);
            break;

        case 2: // 16-bit
            memcpy((char*)&valarray, (pCode + 2), (length - 2));
            value = VSCP_INT16_SWAP_ON_LE(*((uint16_t*)valarray));
            break;

        case 3: // 24-bit
            memcpy(((char*)&valarray + 1), (pCode + 2), (length - 2));
            if (bNegative)
                *valarray = 0xff; // First byte must be 0xff
            value = VSCP_INT32_SWAP_ON_LE(*((int32_t*)valarray));
            break;

        case 4: // 32-bit
            memcpy((char*)&valarray, (pCode + 2), (length - 2));
            value = VSCP_INT32_SWAP_ON_LE(*((int32_t*)valarray));
            break;

        case 5: // 40-bit
            memcpy(((char*)&valarray + 3), (pCode + 2), (length - 2));
            if (bNegative) {
                *valarray       = 0xff; // First byte must be 0xff
                *(valarray + 1) = 0xff;
                *(valarray + 2) = 0xff;
            }

#ifdef __BIG_ENDIAN__
            value = *((int64_t*)valarray);
#else
            value64 = Swap8Bytes(*((int64_t*)valarray));
            value   = value64;
#endif
            break;

        case 6: // 48-bit
            memcpy(((char*)&valarray + 2), (pCode + 2), (length - 2));
            if (bNegative) {
                *valarray       = 0xff; // First byte must be 0xff
                *(valarray + 1) = 0xff;
            }

#ifdef __BIG_ENDIAN__
            value = *((int64_t*)valarray);
#else
            value64 = Swap8Bytes(*((int64_t*)valarray));
            value   = value64;
#endif
            break;
    }

        // Bring back decimal points
#ifdef WIN32
    if (decibyte & 0x80) {
        decibyte &= 0x7f;
        value = value / (pow(10.0, decibyte));
    } else {
        decibyte &= 0x7f;
        value = value * (pow(10.0, decibyte));
    }
#else
    if (decibyte & 0x80) {
        decibyte &= 0x7f;
        value = value / (pow(10, decibyte));
    } else {
        decibyte &= 0x7f;
        value = value * (pow(10, decibyte));
    }
#endif
    return value;
}

//////////////////////////////////////////////////////////////////////////////
// getDataCodingString
//

std::string&
vscp_getDataCodingString(const unsigned char* pCode, unsigned char length)
{

    static std::string str;
    char buf[20];

    str.clear();

    if (NULL != pCode) {
        memset(buf, 0, sizeof(buf));
        memcpy(buf, pCode + 1, length - 1);
        str = std::string(buf);
    }

    return str;
}

//////////////////////////////////////////////////////////////////////////////
// getDataCodingString
//

bool
vscp_getDataCodingString(std::string& strResult,
                         const unsigned char* pCode,
                         unsigned char dataSize)
{
    char buf[20];

    if (NULL == pCode)
        return false;

    strResult.clear();
    if (NULL != pCode) {
        memset(buf, 0, sizeof(buf));
        memcpy(buf, pCode + 1, dataSize - 1);

        strResult = std::string(buf);
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// getMeasurementAsFloat
//

float
vscp_getMeasurementAsFloat(const unsigned char* pCode, unsigned char length)
{
    float* pfloat = NULL;
    float value   = 0.0f;

    // Check pointers
    if (NULL == pCode)
        return false;

    if (length >= 5) {
        pfloat = (float*)(pCode + 1);
        value  = *pfloat;
        // TODO: please insert test for (!NaN || !INF)
    }

    return value;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getMeasurementAsString
//
//

bool
vscp_getMeasurementAsString(std::string& strValue, const vscpEvent* pEvent)
{
    int i, j;
    int offset = 0;

    // Check pointers
    if (NULL == pEvent)
        return false;

    strValue.clear();

    // Check pointers
    if (NULL == pEvent)
        return false;
    if (NULL == pEvent->pdata)
        return false;

    if (VSCP_CLASS2_MEASUREMENT_STR == pEvent->vscp_class) {

        char buf[512];

        memset(buf, 0, sizeof(buf));
        memcpy(buf, pEvent->pdata + 4, pEvent->sizeData - 4);
        strValue = std::string(buf);

    } else if (VSCP_CLASS2_MEASUREMENT_FLOAT == pEvent->vscp_class) {

        uint8_t buf[8];

        // Must be correct data
        if (12 != pEvent->sizeData)
            return false;

        memset(buf, 0, sizeof(buf));
        memcpy(buf, pEvent->pdata + 4, 8); // Double

        // Take care of byte order on little endian
        if (vscp_isLittleEndian()) {

            for (int i = 7; i > 0; i--) {
                buf[i] = buf[7 - i];
            }
        }

        strValue = vscp_str_format("%f", *((double*)buf));

    } else if (VSCP_CLASS1_MEASUREMENT32 == pEvent->vscp_class) {

        uint8_t buf[4];

        // Must be correct data
        if (4 != pEvent->sizeData)
            return false;

        memset(buf, 0, sizeof(buf));
        memcpy(buf, pEvent->pdata, 4); // float

        strValue = vscp_str_format("%f", *((float*)buf));

    } else if (VSCP_CLASS2_LEVEL1_MEASUREMENT32 == pEvent->vscp_class) {

        uint8_t buf[4];

        // Must be correct data
        if ((16 + 4) != pEvent->sizeData)
            return false;

        memset(buf, 0, sizeof(buf));
        memcpy(buf, pEvent->pdata + 16, 4); // float

        strValue = vscp_str_format("%f", *((float*)buf));

    } else if (VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) {

        uint8_t buf[8];

        // Must be correct data
        if (8 != pEvent->sizeData)
            return false;

        memset(buf, 0, sizeof(buf));
        memcpy(buf, pEvent->pdata, 8); // Double

        strValue = vscp_str_format("%f", *((double*)buf));

    } else if (VSCP_CLASS2_LEVEL1_MEASUREMENT64 == pEvent->vscp_class) {

        uint8_t buf[8];

        // Must be correct data
        if ((16 + 8) != pEvent->sizeData)
            return false;

        memset(buf, 0, sizeof(buf));
        memcpy(buf, pEvent->pdata + 16, 8); // Double

        strValue = vscp_str_format("%f", *((double*)buf));

    }

    else if ((VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) ||
             (VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) ||
             (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) ||
             (VSCP_CLASS1_DATA == pEvent->vscp_class) ||
             (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class) ||
             (VSCP_CLASS2_LEVEL1_MEASUREZONE == pEvent->vscp_class) ||
             (VSCP_CLASS2_LEVEL1_SETVALUEZONE == pEvent->vscp_class)) {

        // If class >= 512 and class < 1024 we
        // have GUID in front of data.
        if ((pEvent->vscp_class >= VSCP_CLASS2_LEVEL1_PROTOCOL)) {
            offset = 16;
        }

        // Must be at least two data bytes
        if (pEvent->sizeData - offset < 2)
            return false;

        // Point past index,zone,subzone
        if ((VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) ||
            (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) ||
            (VSCP_CLASS2_LEVEL1_MEASUREZONE == pEvent->vscp_class) ||
            (VSCP_CLASS2_LEVEL1_SETVALUEZONE == pEvent->vscp_class)) {

            offset += 3;
        }

        unsigned short type = (0x07 & (pEvent->pdata[0 + offset] >> 5));

        switch (type) {

            case 0: // series of bits
                for (i = 1; i < (pEvent->sizeData - offset); i++) {

                    for (j = 7; j >= 0; j--) {

                        if (pEvent->pdata[i + offset] & (1 << j)) {
                            strValue += "1";
                        } else {
                            strValue += "0";
                        }
                    }
                    strValue += " ";
                }
                break;

            case 1: // series of bytes
                for (i = 1; i < (pEvent->sizeData - offset); i++) {

                    strValue +=
                      vscp_str_format("%d", pEvent->pdata[i + offset]);

                    if (i != (pEvent->sizeData - 1 - offset)) {
                        strValue += ",";
                    }
                }
                break;

            case 2: // string
            {
                char strData[9];
                memset(strData, 0, sizeof(strData));
                for (i = 1; i < (pEvent->sizeData - offset); i++) {
                    strData[i - 1] = pEvent->pdata[i + offset];
                }

                strValue = std::string(strData);

            } break;

            case 3: // integer
            {
                double value =
                  (double)vscp_getDataCodingInteger(pEvent->pdata + offset,
                                                    pEvent->sizeData - offset);
                strValue = vscp_str_format("%.0lf", value);
            } break;

            case 4: // normalized integer
            {
                double value = vscp_getDataCodingNormalizedInteger(
                  pEvent->pdata + offset,
                  pEvent->sizeData - offset);
                strValue = vscp_str_format("%lf", value);
            } break;

            case 5: // Floating point value
            {
                // s eeeeeeee mmmmmmmmmmmmmmmmmmmmmmm
                // s = sign bit( 1-bit)
                // e = exponent ( 8-bit)
                // m = mantissa (23-bit)
                int sign = 1;
                unsigned char exponent;

                // Check the sign
                if (pEvent->pdata[1 + offset] & 0x80) {
                    sign = -1;
                }

                // Clear the sign bit
                pEvent->pdata[1 + offset] &= 0x7f;

                // Get the exponent
                exponent = (pEvent->pdata[1 + offset] << 1);
                if (pEvent->pdata[2 + offset] & 0x80) {
                    exponent = exponent | 1;
                }

                // Clear the exponent
                pEvent->pdata[1 + offset] = 0;
                pEvent->pdata[2 + offset] &= 0x7f;

                uint32_t value = *((uint32_t*)(pEvent->pdata + 1 + offset));
                value          = VSCP_UINT32_SWAP_ON_LE(value);

                double dValue = value;
                dValue        = sign * (dValue * pow(10.0, exponent));
                strValue      = vscp_str_format("%f", dValue);

            } break;

            case 6: // Not defined yet
                break;

            case 7: // Not defined yet
                break;
        }
    } else {
        return false; // Measurement type is not supported
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getMeasurementAsDouble
//
//

bool
vscp_getMeasurementAsDouble(double* pvalue, const vscpEvent* pEvent)
{
    std::string str;

    // Check pointers
    if (NULL == pEvent)
        return false;
    if (NULL == pvalue)
        return false;

    if ((VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_DATA == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) ||
        (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class)) {

        if (!vscp_getMeasurementAsString(str, pEvent))
            return false;
        *pvalue = stod(str);

    } else if (VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) {

        if (!vscp_getMeasurementFloat64AsString(str, pEvent))
            return false;
        *pvalue = stod(str);

    } else if (VSCP_CLASS2_MEASUREMENT_STR == pEvent->vscp_class) {

        char buf[512];

        if (0 == pEvent->sizeData || NULL == pEvent->pdata)
            return false;
        memcpy(buf, pEvent->pdata + 4, pEvent->sizeData - 4);

        // str = std::string( buf );
        *pvalue = stod(std::string(buf));

    } else if (VSCP_CLASS2_MEASUREMENT_FLOAT == pEvent->vscp_class) {

        char buf[8];

        memcpy(buf, pEvent->pdata + 4, 8);
        *pvalue = *(double*)(buf);

    } else {
        return false;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getMeasurementFloat64AsString
//
//

bool
vscp_getMeasurementFloat64AsString(std::string& strValue,
                                   const vscpEvent* pEvent)
{
    int offset = 0;

    // If class >= 512 and class <1024 we
    // have GUID in front of data.
    if ((pEvent->vscp_class >= VSCP_CLASS2_LEVEL1_PROTOCOL)) {
        offset = 16;
    }

    if (pEvent->sizeData - offset != 8)
        return false;

    double* pfloat = (double*)(pEvent->pdata + offset);
    strValue       = vscp_str_format("%lf", *pfloat);

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getMeasurementWithZoneAsString
//
//

bool
vscp_getMeasurementWithZoneAsString(const vscpEvent* pEvent,
                                    std::string& strValue)
{
    int offset = 0;

    // If class >= 512 and class <1024 we
    // have GUID in front of data.
    if ((pEvent->vscp_class >= VSCP_CLASS2_LEVEL1_PROTOCOL) &&
        (pEvent->vscp_class < VSCP_CLASS2_PROTOCOL)) {
        offset = 16;
    }

    // Must at least have index, zone, subzone, normaliser byte, one data byte
    if (pEvent->sizeData - offset < 5)
        return false;

    // We mimic a standard measurement
    vscpEvent eventMimic;
    eventMimic.pdata      = new uint8_t[pEvent->sizeData - offset - 3];
    eventMimic.vscp_class = pEvent->vscp_class;
    eventMimic.vscp_type  = pEvent->vscp_type;
    eventMimic.sizeData   = pEvent->sizeData;
    memcpy(eventMimic.pdata,
           pEvent->pdata + offset + 3,
           pEvent->sizeData - offset - 3);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_getMeasurementUnit
//

int
vscp_getMeasurementUnit(const vscpEvent* pEvent)
{
    int offset = 0;

    // If class >= 512 and class < 1024 we
    // have GUID in front of data.
    if ((pEvent->vscp_class >= VSCP_CLASS2_LEVEL1_PROTOCOL) &&
        (pEvent->vscp_class < VSCP_CLASS2_PROTOCOL)) {
        offset = 16;
    }

    if ((VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_DATA == pEvent->vscp_class) ||
        (VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREZONE == pEvent->vscp_class) ||
        (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_SETVALUEZONE == pEvent->vscp_class)) {

        if ((NULL == pEvent->pdata) || (pEvent->sizeData < (offset + 1))) {
            return VSCP_ERROR_ERROR;
        }

        return VSCP_DATACODING_UNIT(pEvent->pdata[offset + 0]);

    } else if ((VSCP_CLASS1_MEASUREMENT32 == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREMENT32 == pEvent->vscp_class)) {
        return 0; // Always default unit
    } else if ((VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREMENT64 == pEvent->vscp_class)) {
        return 0; // Always default unit
    } else if ((VSCP_CLASS2_MEASUREMENT_STR == pEvent->vscp_class)) {

        // Check if data length is valid
        if ((NULL == pEvent->pdata) || (pEvent->sizeData < 4))
            return VSCP_ERROR_ERROR;

        return pEvent->pdata[3];

    } else if ((VSCP_CLASS2_MEASUREMENT_FLOAT == pEvent->vscp_class)) {

        // Check if data length is valid
        if ((NULL == pEvent->pdata) || (12 != pEvent->sizeData))
            return VSCP_ERROR_ERROR;

        return pEvent->pdata[3];
    }

    return VSCP_ERROR_ERROR;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_getMeasurementSensorIndex
//

int
vscp_getMeasurementSensorIndex(const vscpEvent* pEvent)
{
    int offset = 0;

    // If class >= 512 and class < 1024 we
    // have GUID in front of data.
    if ((pEvent->vscp_class >= VSCP_CLASS2_LEVEL1_PROTOCOL) &&
        (pEvent->vscp_class < VSCP_CLASS2_PROTOCOL)) {
        offset = 16;
    }

    if ((VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_DATA == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class)) {

        return VSCP_DATACODING_INDEX(pEvent->pdata[offset + 0]);

    } else if ((VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREZONE == pEvent->vscp_class) ||
               (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_SETVALUEZONE == pEvent->vscp_class)) {

        if ((NULL == pEvent->pdata) || (pEvent->sizeData >= (offset + 3)))
            return 0;

        return pEvent->pdata[offset + 0];
    } else if ((VSCP_CLASS1_MEASUREMENT32 == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREMENT32 == pEvent->vscp_class)) {
        return 0; // Sensor index is always zero
    } else if ((VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREMENT64 == pEvent->vscp_class)) {
        return 0; // Sensor index is always zero
    } else if ((VSCP_CLASS2_MEASUREMENT_STR == pEvent->vscp_class)) {

        // Check if data length is valid
        if ((NULL == pEvent->pdata) || (pEvent->sizeData < 4))
            return VSCP_ERROR_ERROR;

        return pEvent->pdata[0];

    } else if ((VSCP_CLASS2_MEASUREMENT_FLOAT == pEvent->vscp_class)) {

        // Check if data length is valid
        if ((NULL == pEvent->pdata) || (12 != pEvent->sizeData))
            return VSCP_ERROR_ERROR;

        return pEvent->pdata[0];
    }

    return VSCP_ERROR_ERROR;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_getMeasurementZone
//

int
vscp_getMeasurementZone(const vscpEvent* pEvent)
{
    int offset = 0;

    // If class >= 512 and class < 1024 we
    // have GUID in front of data.
    if ((pEvent->vscp_class >= VSCP_CLASS2_LEVEL1_PROTOCOL) &&
        (pEvent->vscp_class < VSCP_CLASS2_PROTOCOL)) {
        offset = 16;
    }

    if ((VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_DATA == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class)) {

        return 0; // Always zero

    } else if ((VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREZONE == pEvent->vscp_class) ||
               (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_SETVALUEZONE == pEvent->vscp_class)) {

        if ((NULL == pEvent->pdata) || (pEvent->sizeData >= (offset + 3)))
            return 0;

        return pEvent->pdata[offset + 1];
    } else if ((VSCP_CLASS1_MEASUREMENT32 == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREMENT32 == pEvent->vscp_class)) {
        return 0; // Sensor index is always zero
    } else if ((VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREMENT64 == pEvent->vscp_class)) {
        return 0; // Sensor index is always zero
    } else if ((VSCP_CLASS2_MEASUREMENT_STR == pEvent->vscp_class)) {

        // Check if data length is valid
        if ((NULL == pEvent->pdata) || (pEvent->sizeData < 4))
            return VSCP_ERROR_ERROR;

        return pEvent->pdata[2];

    } else if ((VSCP_CLASS2_MEASUREMENT_FLOAT == pEvent->vscp_class)) {

        // Check if data length is valid
        if ((NULL == pEvent->pdata) || (12 != pEvent->sizeData))
            return VSCP_ERROR_ERROR;

        return pEvent->pdata[2];
    }

    return VSCP_ERROR_ERROR;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_getMeasurementSubZone
//

int
vscp_getMeasurementSubZone(const vscpEvent* pEvent)
{
    int offset = 0;

    // If class >= 512 and class < 1024 we
    // have GUID in front of data.
    if ((pEvent->vscp_class >= VSCP_CLASS2_LEVEL1_PROTOCOL) &&
        (pEvent->vscp_class < VSCP_CLASS2_PROTOCOL)) {
        offset = 16;
    }

    if ((VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_DATA == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREZONE == pEvent->vscp_class) ||
        (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_SETVALUEZONE == pEvent->vscp_class)) {

        if ((NULL == pEvent->pdata) || (pEvent->sizeData >= (offset + 1)))
            return 0;

        return 0; // Always zero

    } else if ((VSCP_CLASS1_MEASUREMENT32 == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREMENT32 == pEvent->vscp_class)) {
        return 0; // Sensor index is always zero
    } else if ((VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) ||
               (VSCP_CLASS2_LEVEL1_MEASUREMENT64 == pEvent->vscp_class)) {
        return 0; // Sensor index is always zero
    } else if ((VSCP_CLASS2_MEASUREMENT_STR == pEvent->vscp_class)) {

        // Check if data length is valid
        if ((NULL == pEvent->pdata) || (pEvent->sizeData < 4))
            return VSCP_ERROR_ERROR;

        return pEvent->pdata[offset + 2];

    } else if ((VSCP_CLASS2_MEASUREMENT_FLOAT == pEvent->vscp_class)) {

        // Check if data length is valid
        if ((NULL == pEvent->pdata) || (12 != pEvent->sizeData))
            return VSCP_ERROR_ERROR;

        return pEvent->pdata[2];
    }

    return VSCP_ERROR_ERROR;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_isMeasurement
//

bool
vscp_isMeasurement(const vscpEvent* pEvent)
{
    if ((VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_DATA == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class) ||
        (VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREZONE == pEvent->vscp_class) ||
        (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_SETVALUEZONE == pEvent->vscp_class) ||
        (VSCP_CLASS1_MEASUREMENT32 == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREMENT32 == pEvent->vscp_class) ||
        (VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) ||
        (VSCP_CLASS2_LEVEL1_MEASUREMENT64 == pEvent->vscp_class) ||
        (VSCP_CLASS2_MEASUREMENT_STR == pEvent->vscp_class) ||
        (VSCP_CLASS2_MEASUREMENT_FLOAT == pEvent->vscp_class)) {
        return true;
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////////
// convertFloatToNormalizedEventData
//

bool
vscp_convertFloatToNormalizedEventData(uint8_t* pdata,
                                       uint16_t* psize,
                                       double value,
                                       uint8_t unit,
                                       uint8_t sensoridx)
{
    // Check pointer
    if (NULL == pdata)
        return false;
    if (NULL == psize)
        return false;

    // No data assigned yet
    *psize = 0;

    unit &= 3;  // Mask of invalid bits
    unit <<= 3; // Shift to correct position

    sensoridx &= VSCP_MASK_DATACODING_INDEX; // Mask of invalid bits

    char buf[128];
    bool bNegative = (value > 0) ? false : true;
    UNUSED(bNegative);
    int ndigits = 0;
    uint64_t val64;
    double intpart;
#ifdef WIN32
    _snprintf(buf, sizeof(buf), "%g", value);
#else
    snprintf(buf, sizeof(buf), "%g", value);
#endif
    char* pos = strchr(buf, '.');
    if (NULL != pos) {
        ndigits = strlen(pos) - 1;
    } else {
        pos = strchr(buf, ',');
        if (NULL != pos) {
            ndigits = strlen(pos) - 1;
        } else {
            ndigits = 0;
        }
    }

    (void)modf(value, &intpart);
    val64 = (uint64_t)(value * pow(10.0, ndigits));

    if (val64 < ((double)0x80)) {
        *psize   = 3;
        pdata[2] = val64 & 0xff;
    } else if (val64 < ((double)0x8000)) {
        *psize   = 4;
        pdata[2] = (val64 >> 8) & 0xff;
        pdata[3] = val64 & 0xff;
    } else if (val64 < ((double)0x800000)) {
        *psize   = 5;
        pdata[2] = (val64 >> 16) & 0xff;
        pdata[3] = (val64 >> 8) & 0xff;
        pdata[4] = val64 & 0xff;
    } else if (val64 < ((double)0x80000000)) {
        *psize   = 6;
        pdata[2] = (val64 >> 24) & 0xff;
        pdata[3] = (val64 >> 16) & 0xff;
        pdata[4] = (val64 >> 8) & 0xff;
        pdata[5] = val64 & 0xff;
    } else if (val64 < ((double)0x8000000000)) {
        *psize   = 7;
        pdata[2] = (val64 >> 32) & 0xff;
        pdata[3] = (val64 >> 24) & 0xff;
        pdata[4] = (val64 >> 16) & 0xff;
        pdata[5] = (val64 >> 8) & 0xff;
        pdata[6] = val64 & 0xff;
    } else if (val64 < ((double)0x800000000000)) {
        *psize   = 8;
        pdata[2] = (val64 >> 40) & 0xff;
        pdata[3] = (val64 >> 32) & 0xff;
        pdata[4] = (val64 >> 24) & 0xff;
        pdata[5] = (val64 >> 16) & 0xff;
        pdata[6] = (val64 >> 8) & 0xff;
        pdata[7] = val64 & 0xff;
    } else {
        return false;
    }

    pdata[0] = VSCP_DATACODING_NORMALIZED + unit +
               sensoridx; // Normalised integer + unit + sensor index
    pdata[1] = VSCP_DATACODING_NORMALIZED +
               ndigits; // Decimal point shifted five steps to the left

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_convertFloatToFloatEventData
//

bool
vscp_convertFloatToFloatEventData(uint8_t* pdata,
                                  uint16_t* psize,
                                  float value,
                                  uint8_t unit,
                                  uint8_t sensoridx)
{
    // Max and min for Single-precision floating-point IEEE 754-1985
    double float_max = 3.4e38;
    double float_min = -3.4e38;

    // Check pointer
    if (NULL == pdata)
        return false;
    if (NULL == psize)
        return false;

    if (value > float_max)
        return false;
    if (value < float_min)
        return false;

    // We must make sure
    if (4 != sizeof(float))
        return false;

    void* p    = (void*)&value;
    // uint32_t n = VSCP_UINT32_SWAP_ON_LE(*((uint32_t*)p));
    // float f    = *((float*)((uint8_t*)&n));
    // UNUSED(f);
    p = (void*)&value;

    *psize   = 5;
    pdata[0] = VSCP_DATACODING_SINGLE + (unit << 3) +
               sensoridx; // float + unit + sensor index
    memcpy(pdata + 1, p, 4);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_convertIntegerToNormalizedEventData
//

bool
vscp_convertIntegerToNormalizedEventData(uint8_t* pdata,
                                         uint16_t* psize,
                                         uint64_t val64,
                                         uint8_t unit,
                                         uint8_t sensoridx)
{
    uint8_t i;
    uint8_t data[8];

    uint8_t* p = (uint8_t*)&val64;

    if (vscp_isLittleEndian()) {
        for (i = 7; i > 0; i--) {
            data[7 - i] = *(p + i);
        }
    } else {
        memcpy(data, (uint8_t*)val64, 8);
    }

    // Count the leading zeror
    uint8_t nZeros = 0;
    for (i = 0; i < 8; i++) {
        if (*(p + i))
            break;
        nZeros++;
    }

    *psize  = 1; // Size will be at least one byte
    int pos = 0;
    for (i = nZeros; i < 8; i++) {
        pdata[pos + 1] = *(p + i + nZeros);
        (*psize)++;
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_makeFloatMeasurementEvent
//

bool
vscp_makeFloatMeasurementEvent(vscpEvent* pEvent,
                               float value,
                               uint8_t unit,
                               uint8_t sensoridx)
{
    uint8_t offset = 0;

    // Allocate data if needed
    if ((NULL == pEvent->pdata) &&
        (VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class)) {
        offset        = 0;
        pEvent->pdata = new uint8_t[5];
        if (NULL == pEvent->pdata)
            return false;
    } else if ((NULL == pEvent->pdata) &&
               (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class)) {
        offset        = 16;
        pEvent->pdata = new uint8_t[16 + 5];
        if (NULL == pEvent->pdata)
            return false;
    } else {
        return false;
    }

    return vscp_convertFloatToFloatEventData(pEvent->pdata + offset,
                                             &pEvent->sizeData,
                                             value,
                                             unit,
                                             sensoridx);
}

//////////////////////////////////////////////////////////////////////////////
// vscp_makeStringMeasurementEvent
//

bool
vscp_makeStringMeasurementEvent(vscpEvent* pEvent,
                                double value,
                                uint8_t unit,
                                uint8_t sensoridx)
{
    uint8_t offset = 0;
    std::string strValue;

    unit &= 3;  // Mask of invalid bits
    unit <<= 3; // Shift to correct position

    sensoridx &= 7; // Mask of invalid bits

    strValue         = vscp_str_format("%f", value);
    pEvent->sizeData = (strValue.length() > 7) ? 8 : (strValue.length() + 1);

    // Allocate data if needed
    if ((NULL == pEvent->pdata) &&
        (VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class)) {
        offset        = 0;
        pEvent->pdata = new uint8_t[pEvent->sizeData + 1];
        if (NULL == pEvent->pdata)
            return false;
    } else if ((NULL == pEvent->pdata) &&
               (VSCP_CLASS2_LEVEL1_MEASUREMENT == pEvent->vscp_class)) {
        offset        = 16;
        pEvent->pdata = new uint8_t[16 + pEvent->sizeData + 1];
        if (NULL == pEvent->pdata)
            return false;
    } else {
        return false;
    }

    pEvent->pdata[offset + 0] =
      VSCP_DATACODING_STRING + unit + sensoridx; // float + unit + sensorindex
    memcpy(pEvent->pdata + offset + 1, strValue.c_str(), pEvent->sizeData - 1);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_makeLevel2FloatMeasurementEvent
//

bool
vscp_makeLevel2FloatMeasurementEvent(vscpEvent* pEvent,
                                     uint16_t type,
                                     double value,
                                     uint8_t unit,
                                     uint8_t sensoridx,
                                     uint8_t zone,
                                     uint8_t subzone)
{
    // Event must have been created
    if (NULL == pEvent)
        return false;

    pEvent->vscp_class = VSCP_CLASS2_MEASUREMENT_FLOAT;
    pEvent->vscp_type  = type;
    pEvent->obid       = 0;
    pEvent->timestamp  = 0;

    pEvent->sizeData = 12;
    pEvent->pdata    = new uint8_t[pEvent->sizeData];
    if (NULL == pEvent->pdata) {
        delete pEvent;
        return false;
    }

    // Copy in data
    pEvent->pdata[0] = sensoridx;
    pEvent->pdata[1] = zone;
    pEvent->pdata[2] = subzone;
    pEvent->pdata[3] = unit;
    memcpy((pEvent->pdata + 4), (unsigned char*)&value, 8);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_makeLevel2StringMeasurementEvent
//

bool
vscp_makeLevel2StringMeasurementEvent(vscpEvent* pEvent,
                                      uint16_t type,
                                      double value,
                                      uint8_t unit,
                                      uint8_t sensoridx,
                                      uint8_t zone,
                                      uint8_t subzone)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    std::string strData = vscp_str_format("%f", value);

    pEvent->vscp_class = VSCP_CLASS2_MEASUREMENT_STR;
    pEvent->vscp_type  = type;
    pEvent->obid       = 0;
    pEvent->timestamp  = 0;
    memset(pEvent->GUID, 0, 16);
    pEvent->sizeData =
      4 + strlen(strData.c_str()) + 1; // Include null termination

    pEvent->pdata = new uint8_t[pEvent->sizeData];
    if (NULL == pEvent->pdata) {
        delete pEvent;
        return false;
    }

    // Nill
    memset(pEvent->pdata, 0, pEvent->sizeData);

    // Copy in data
    pEvent->pdata[0] = sensoridx;
    pEvent->pdata[1] = zone;
    pEvent->pdata[2] = subzone;
    pEvent->pdata[3] = unit;
    memcpy((pEvent->pdata + 4), strData.c_str(), pEvent->sizeData);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_convertLevel1MeasuremenToLevel2Double
//

bool
vscp_convertLevel1MeasuremenToLevel2Double(vscpEvent* pEvent)
{
    double val64;

    // Check pointers
    if (NULL == pEvent)
        return false;
    if (NULL == pEvent->pdata)
        return false;

    // Must be a measurement event
    if (!vscp_isMeasurement(pEvent))
        return false;

    if (vscp_getMeasurementAsDouble(&val64, pEvent)) {

        pEvent->vscp_class = VSCP_CLASS2_MEASUREMENT_FLOAT;
        uint8_t* p         = new uint8_t[12];
        if (NULL != p) {

            memset(p, 0, 12);
            /*
            0 	Index for sensor, 0-255.
            1 	Zone, 0-255.
            2 	Sub zone, 0-255.
            3 	Unit from measurements, 0-255.
            4-11 	64-bit double precision floating point value stored MSB
            first.
             */
            if (VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) {

                // Sensor index
                p[0] = pEvent->pdata[0] & VSCP_MASK_DATACODING_INDEX;

                // Zone + Subzone
                p[1] = p[2] = 0x00;

                // unit
                p[3] = (pEvent->pdata[0] & VSCP_MASK_DATACODING_UNIT) >> 3;

                // Floating point value
                val64 = VSCP_UINT64_SWAP_ON_LE(val64);
                memcpy(p + 4, &val64, sizeof(val64));

                delete[] pEvent->pdata;

                pEvent->pdata = p;

            } else if (VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) {

                // Index = 0, Unit = 0, Zone = 0, Subzone = 0
                // Floating point value
                val64 = VSCP_UINT64_SWAP_ON_LE(val64);
                memcpy(p + 4, &val64, sizeof(val64));

                delete[] pEvent->pdata;

                pEvent->pdata = p;

            } else if (VSCP_CLASS1_MEASUREMENT32 == pEvent->vscp_class) {

                // Index = 0, Unit = 0, Zone = 0, Subzone = 0
                // Floating point value
                val64 = VSCP_UINT64_SWAP_ON_LE(val64);
                memcpy(p + 4, &val64, sizeof(val64));

                delete[] pEvent->pdata;

                pEvent->pdata = p;

            } else if (VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) {

                // Sensor index
                p[0] = pEvent->pdata[0];

                // Zone
                p[1] = pEvent->pdata[1];

                // Subzone
                p[2] = pEvent->pdata[2];

                val64 = VSCP_UINT64_SWAP_ON_LE(val64);
                memcpy(p + 4, &val64, sizeof(val64));

                delete[] pEvent->pdata;

                pEvent->pdata = p;

            } else if (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) {

                // Sensor index
                p[0] = pEvent->pdata[0];

                // Zone
                p[1] = pEvent->pdata[1];

                // Subzone
                p[2] = pEvent->pdata[2];

                val64 = VSCP_UINT64_SWAP_ON_LE(val64);
                memcpy(p + 4, &val64, sizeof(val64));

                delete[] pEvent->pdata;

                pEvent->pdata = p;

            } else {
                delete[] p;
                p = NULL;
                return false; // Not a measurement event, hmmmm.... strange
            }
        } else {
            return false; // Unable to allocate data
        }
    } else {
        return false; // Could not get value
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_convertLevel1MeasuremenToLevel2String
//

bool
vscp_convertLevel1MeasuremenToLevel2String(vscpEvent* pEvent)
{
    std::string strval;

    // Check pointers
    if (NULL == pEvent)
        return false;
    if (NULL == pEvent->pdata)
        return false;

    // Must be a measurement event
    if (!vscp_isMeasurement(pEvent))
        return false;

    if (vscp_getMeasurementAsString(strval, pEvent)) {

        pEvent->vscp_class = VSCP_CLASS2_MEASUREMENT_STR;

        char* p = new char[4 + strval.length()];
        if (NULL != p) {

            memset(p, 0, 4 + strval.length());

            if (VSCP_CLASS1_MEASUREMENT == pEvent->vscp_class) {

                // Sensor index
                p[0] = pEvent->pdata[0] & VSCP_MASK_DATACODING_INDEX;

                // Zone + Subzone
                p[1] = p[2] = 0x00;

                // unit
                p[3] = (pEvent->pdata[0] & VSCP_MASK_DATACODING_UNIT) >> 3;

                // Copy in the value string
                strcpy(p + 4, (const char*)strval.c_str());

                delete[] pEvent->pdata;

                pEvent->pdata = (uint8_t*)p;

            } else if (VSCP_CLASS1_MEASUREMENT64 == pEvent->vscp_class) {

                // Index = 0, Unit = 0, Zone = 0, Subzone = 0
                // Floating point value
                // Copy in the value string
                strcpy(p + 4, (const char*)strval.c_str());

                delete[] pEvent->pdata;

                pEvent->pdata = (uint8_t*)p;

            } else if (VSCP_CLASS1_MEASUREMENT32 == pEvent->vscp_class) {

                // Index = 0, Unit = 0, Zone = 0, Subzone = 0
                // Floating point value
                // Copy in the value string
                strcpy(p + 4, (const char*)strval.c_str());

                delete[] pEvent->pdata;

                pEvent->pdata = (uint8_t*)p;

            } else if (VSCP_CLASS1_MEASUREZONE == pEvent->vscp_class) {

                // Sensor index
                p[0] = pEvent->pdata[0];

                // Zone
                p[1] = pEvent->pdata[1];

                // Subzone
                p[2] = pEvent->pdata[2];

                // Copy in the value string
                strcpy(p + 4, (const char*)strval.c_str());

                delete[] pEvent->pdata;

                pEvent->pdata = (uint8_t*)p;

            } else if (VSCP_CLASS1_SETVALUEZONE == pEvent->vscp_class) {

                // Sensor index
                p[0] = pEvent->pdata[0];

                // Zone
                p[1] = pEvent->pdata[1];

                // Subzone
                p[2] = pEvent->pdata[2];

                // Copy in the value string
                strcpy(p + 4, (const char*)strval.c_str());

                delete[] pEvent->pdata;

                pEvent->pdata = (uint8_t*)p;

            } else {
                delete[] p;
                p = NULL;
                return false; // Not a measurement.... hmm.... strange
            }
        } else {
            return false; // Unable to allocate data
        }
    } else {
        return false; // Could not get value
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// replaceBackslash
//

std::string&
vscp_replaceBackslash(std::string& str)
{
    size_t pos;
    while (str.npos != (pos = str.find('\\'))) {
        str[pos] = '/';
    }

    return str;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getEventPriority

unsigned char
vscp_getEventPriority(const vscpEvent* pEvent)
{
    // Must be a valid message pointer
    if (NULL == pEvent)
        return 0;

    return ((pEvent->head >> 5) & 0x07);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getEventExPriority

unsigned char
vscp_getEventExPriority(const vscpEventEx* pEvent)
{
    // Must be a valid message pointer
    if (NULL == pEvent)
        return 0;

    return ((pEvent->head >> 5) & 0x07);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_setEventPriority

void
vscp_setEventPriority(vscpEvent* pEvent, unsigned char priority)
{
    // Must be a valid message pointer
    if (NULL == pEvent)
        return;

    pEvent->head &= ~VSCP_HEADER_PRIORITY_MASK;
    pEvent->head |= (priority << 5);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_setEventExPriority

void
vscp_setEventExPriority(vscpEventEx* pEvent, unsigned char priority)
{
    // Must be a valid message pointer
    if (NULL == pEvent)
        return;

    pEvent->head &= ~VSCP_HEADER_PRIORITY_MASK;
    pEvent->head |= (priority << 5);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getHeadFromCANALid
//

unsigned char
vscp_getHeadFromCANALid(uint32_t id)
{
    uint8_t hardcoded = 0;
    uint8_t priority  = (0x07 & (id >> 26));
    if (id & (1 << 25))
        hardcoded = VSCP_HEADER_HARD_CODED;
    return ((priority << 5) | hardcoded);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getVscpClassFromCANALid
//

uint16_t
vscp_getVscpClassFromCANALid(uint32_t id)
{
    return (uint16_t)(0x1ff & (id >> 16));
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getVscpTypeFromCANALid
//

uint16_t
vscp_getVscpTypeFromCANALid(uint32_t id)
{
    return (uint16_t)(0xff & (id >> 8));
}

///////////////////////////////////////////////////////////////////////////////
// getNicknameFromCANALid
//

uint8_t
vscp_getNicknameFromCANALid(uint32_t id)
{
    return (id & 0xff);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getCANALidFromData
//

uint32_t
vscp_getCANALidFromData(unsigned char priority,
                        const uint16_t vscp_class,
                        const uint16_t vscp_type)
{
    // unsigned long t1 = (unsigned long)priority << 20;
    // unsigned long t2 = (unsigned long)pvscpMsg->vscp_class << 16;
    // unsigned long t3 = (unsigned long)pvscpMsg->vscp_type << 8;
    return (((unsigned long)priority << 26) |
            ((unsigned long)vscp_class << 16) |
            ((unsigned long)vscp_type << 8) | 0);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getCANALidFromEvent
//

uint32_t
vscp_getCANALidFromEvent(const vscpEvent* pEvent)
{
    return (((unsigned long)vscp_getEventPriority(pEvent) << 26) |
            ((unsigned long)pEvent->vscp_class << 16) |
            ((unsigned long)pEvent->vscp_type << 8) | 0);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getCANALidFromEventEx
//

uint32_t
vscp_getCANALidFromEventEx(const vscpEventEx* pEvent)
{
    return (((unsigned long)vscp_getEventExPriority(pEvent) << 26) |
            ((unsigned long)pEvent->vscp_class << 16) |
            ((unsigned long)pEvent->vscp_type << 8) | 0);
}

///////////////////////////////////////////////////////////////////////////////
// vscp_calc_crc_Event
//
// Calculate CRC for Level 2 package. If bSet is true also write the
// crc into the packet.
//

unsigned short
vscp_calc_crc_Event(vscpEvent* pEvent, short bSet)
{
    unsigned short crc = 0;
    unsigned char* p;
    unsigned char* pbuf;

    // Must be a valid message pointer
    if (NULL == pEvent)
        return 0;

    crcInit();

    pbuf = (unsigned char*)malloc(23 + pEvent->sizeData);

    if (NULL != pbuf) {

        p = pbuf;

        memcpy(p, (unsigned char*)&pEvent->head, 1);
        p++;

        memcpy(p, (unsigned char*)&pEvent->vscp_class, 2);
        p += 2;

        memcpy(p, (unsigned char*)&pEvent->vscp_type, 2);
        p += 2;

        memcpy(p, (unsigned char*)&pEvent->GUID, 16);
        p += 16;

        memcpy(p, pEvent->pdata, pEvent->sizeData);
        p += pEvent->sizeData;

        memcpy(p, (unsigned char*)&pEvent->sizeData, 2);
        p += 2;

        crc = crcFast(pbuf, sizeof(pbuf));

        if (bSet)
            pEvent->crc = crc;

        free(pbuf);
    }

    return crc;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_calc_crc_EventEx
//
// Calculate CRC for Level 2 package. If bSet is true also write the
// crc into the packet.
//

unsigned short
vscp_calc_crc_EventEx(vscpEventEx* pEvent, short bSet)
{
    unsigned short crc = 0;
    unsigned char* p;
    unsigned char* pbuf;

    // Must be a valid message pointer
    if (NULL == pEvent)
        return 0;

    crcInit();

    pbuf = (unsigned char*)malloc(23 + pEvent->sizeData);

    if (NULL != pbuf) {

        p = pbuf;

        memcpy(p, (unsigned char*)&pEvent->head, 1);
        p++;

        memcpy(p, (unsigned char*)&pEvent->vscp_class, 2);
        p += 2;

        memcpy(p, (unsigned char*)&pEvent->vscp_type, 2);
        p += 2;

        memcpy(p, (unsigned char*)&pEvent->GUID, 16);
        p += 16;

        memcpy(p, pEvent->data, pEvent->sizeData);
        p += pEvent->sizeData;

        memcpy(p, (unsigned char*)&pEvent->sizeData, 2);
        p += 2;

        crc = crcFast(pbuf, sizeof(pbuf));

        if (bSet)
            pEvent->crc = crc;

        free(pbuf);
    }

    return crc;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_calcCRC4GUIDArray
//

uint8_t
vscp_calcCRC4GUIDArray(const uint8_t* pguid)
{
    uint8_t crc = 0;

    init_crc8();
    for (int i = 0; i < 16; i++) {
        crc8(&crc, pguid[i]);
    }

    return crc;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_calcCRC4GUIDString
//

uint8_t
vscp_calcCRC4GUIDString(const std::string& strguid)
{
    uint8_t crc = 0;
    uint8_t guid[16];

    memset(guid, 0, 16);
    vscp_getGuidFromStringToArray(guid, strguid);

    init_crc8();
    for (int i = 0; i < 16; i++) {
        crc8(&crc, guid[i]);
    }

    return crc;
}

///////////////////////////////////////////////////////////////////////////////
// setEventGuidFromString
//

bool
vscp_setEventGuidFromString(vscpEvent* pEvent, const std::string& strGUID)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    std::string str = strGUID;
    vscp_trim(str);
    if ((0 == str.length()) || (str == "-")) {
        memset(pEvent->GUID, 0, 16);
    } else {
        std::deque<std::string> tokens;
        vscp_split(tokens, strGUID, ":");
        for (int i = 0; i < (int)MIN(16, tokens.size()); i++) {
            pEvent->GUID[i] =
              (uint8_t)stol(tokens.front().c_str(), nullptr, 16);
            tokens.pop_front();
            // If no tokens left no use to continue
            if (tokens.size())
                break;
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_setEventExGuidFromString
//

bool
vscp_setEventExGuidFromString(vscpEventEx* pEvent, const std::string& strGUID)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    memset(pEvent->GUID, 0, 16);

    if (strGUID == "-") {
        return true;
    } else {
        std::deque<std::string> tokens;
        vscp_split(tokens, strGUID, ":");
        for (int i = 0; i < 16; i++) {
            pEvent->GUID[i] =
              (uint8_t)stol(tokens.front().c_str(), nullptr, 16);
            tokens.pop_front();
            // If no tokens left no use to continue
            if (tokens.size())
                break;
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getGuidFromStringToArray
//

bool
vscp_getGuidFromStringToArray(unsigned char* pGUID, const std::string& strGUID)
{
    std::string str = vscp_trim_copy(strGUID);

    if (NULL == pGUID) {
        return false;
    }

    // If GUID is empty or "-" set all to zero
    if ((0 == str.length()) || (0 == str.compare("-"))) {
        memset(pGUID, 0, 16);
        return true;
    }

    uint8_t cnt = 0;
    std::deque<std::string> tokens;
    vscp_split(tokens, strGUID, ":");
    while (tokens.size()) {
        if (cnt > 15)
            return false;
        std::size_t pos;
        pGUID[cnt++] = (uint8_t)std::stoul(tokens.front(), &pos, 16);
        tokens.pop_front();
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// writeGuidToString
//

bool
vscp_writeGuidToString(std::string& strGUID, const vscpEvent* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    strGUID = vscp_str_format("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%"
                              "02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                              pEvent->GUID[0],
                              pEvent->GUID[1],
                              pEvent->GUID[2],
                              pEvent->GUID[3],
                              pEvent->GUID[4],
                              pEvent->GUID[5],
                              pEvent->GUID[6],
                              pEvent->GUID[7],
                              pEvent->GUID[8],
                              pEvent->GUID[9],
                              pEvent->GUID[10],
                              pEvent->GUID[11],
                              pEvent->GUID[12],
                              pEvent->GUID[13],
                              pEvent->GUID[14],
                              pEvent->GUID[15]);

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// writeGuidToStringEx
//

bool
vscp_writeGuidToStringEx(std::string& strGUID, const vscpEventEx* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    strGUID = vscp_str_format("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%"
                              "02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                              pEvent->GUID[0],
                              pEvent->GUID[1],
                              pEvent->GUID[2],
                              pEvent->GUID[3],
                              pEvent->GUID[4],
                              pEvent->GUID[5],
                              pEvent->GUID[6],
                              pEvent->GUID[7],
                              pEvent->GUID[8],
                              pEvent->GUID[9],
                              pEvent->GUID[10],
                              pEvent->GUID[11],
                              pEvent->GUID[12],
                              pEvent->GUID[13],
                              pEvent->GUID[14],
                              pEvent->GUID[15]);

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// writeGuidToString4Rows
//

bool
vscp_writeGuidToString4Rows(std::string& strGUID, const vscpEvent* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    strGUID = vscp_str_format("%02X:%02X:%02X:%02X\n%02X:%02X:%02X:%02X\n%"
                              "02X:%02X:%02X:%02X\n%02X:%02X:%02X:%02X",
                              pEvent->GUID[0],
                              pEvent->GUID[1],
                              pEvent->GUID[2],
                              pEvent->GUID[3],
                              pEvent->GUID[4],
                              pEvent->GUID[5],
                              pEvent->GUID[6],
                              pEvent->GUID[7],
                              pEvent->GUID[8],
                              pEvent->GUID[9],
                              pEvent->GUID[10],
                              pEvent->GUID[11],
                              pEvent->GUID[12],
                              pEvent->GUID[13],
                              pEvent->GUID[14],
                              pEvent->GUID[15]);

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// writeGuidToString4RowsEx
//

bool
vscp_writeGuidToString4RowsEx(std::string& strGUID, const vscpEventEx* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    strGUID = vscp_str_format("%02X:%02X:%02X:%02X\n%02X:%02X:%02X:%02X\n%"
                              "02X:%02X:%02X:%02X\n%02X:%02X:%02X:%02X",
                              pEvent->GUID[0],
                              pEvent->GUID[1],
                              pEvent->GUID[2],
                              pEvent->GUID[3],
                              pEvent->GUID[4],
                              pEvent->GUID[5],
                              pEvent->GUID[6],
                              pEvent->GUID[7],
                              pEvent->GUID[8],
                              pEvent->GUID[9],
                              pEvent->GUID[10],
                              pEvent->GUID[11],
                              pEvent->GUID[12],
                              pEvent->GUID[13],
                              pEvent->GUID[14],
                              pEvent->GUID[15]);

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// writeGuidToString
//

bool
vscp_writeGuidArrayToString(std::string& strGUID, const unsigned char* pGUID)
{
    // Check pointer
    if (NULL == pGUID)
        return false;

    strGUID = vscp_str_format("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%"
                              "02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                              pGUID[0],
                              pGUID[1],
                              pGUID[2],
                              pGUID[3],
                              pGUID[4],
                              pGUID[5],
                              pGUID[6],
                              pGUID[7],
                              pGUID[8],
                              pGUID[9],
                              pGUID[10],
                              pGUID[11],
                              pGUID[12],
                              pGUID[13],
                              pGUID[14],
                              pGUID[15]);

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// isGUIDEmpty
//

bool
vscp_isGUIDEmpty(const unsigned char* pGUID)
{
    // Check pointers
    if (NULL == pGUID)
        return false;

    return !(pGUID[0] + pGUID[1] + pGUID[2] + pGUID[3] + pGUID[4] + pGUID[5] +
             pGUID[6] + pGUID[7] + pGUID[8] + pGUID[9] + pGUID[10] + pGUID[11] +
             pGUID[12] + pGUID[13] + pGUID[14] + pGUID[15]);
}

//////////////////////////////////////////////////////////////////////////////
// isSameGUID
//

bool
vscp_isSameGUID(const unsigned char* pGUID1, const unsigned char* pGUID2)
{
    // First check pointers
    if (NULL == pGUID1)
        return false;
    if (NULL == pGUID2)
        return false;

    if (0 != memcmp(pGUID1, pGUID2, 16))
        return false;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// reverseGUID
//

bool
vscp_reverseGUID(unsigned char* pGUID)
{
    uint8_t copyGUID[16];

    // First check pointers
    if (NULL == pGUID)
        return false;

    for (int i = 0; i < 16; i++) {
        copyGUID[i] = pGUID[15 - i];
    }

    memcpy(pGUID, copyGUID, 16);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertEventToEventEx

bool
vscp_convertEventToEventEx(vscpEventEx* pEventEx, const vscpEvent* pEvent)
{
    // Check pointers
    if (NULL == pEvent)
        return false;
    if (NULL == pEventEx)
        return false;

    if (pEvent->sizeData > VSCP_LEVEL2_MAXDATA)
        return false;

    // Convert
    pEventEx->crc        = pEvent->crc;
    pEventEx->obid       = pEvent->obid;
    pEventEx->timestamp  = pEvent->timestamp;
    pEventEx->head       = pEvent->head;
    pEventEx->obid       = pEvent->obid;
    pEventEx->vscp_class = pEvent->vscp_class;
    pEventEx->vscp_type  = pEvent->vscp_type;
    pEventEx->sizeData   = pEvent->sizeData;

    memcpy(pEventEx->GUID, pEvent->GUID, 16);
    memcpy(pEventEx->data, pEvent->pdata, pEvent->sizeData);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertEventExToEvent
//

bool
vscp_convertEventExToEvent(vscpEvent* pEvent, const vscpEventEx* pEventEx)
{
    // Check pointers
    if (NULL == pEvent)
        return false;
    if (NULL == pEventEx)
        return false;

    if (pEventEx->sizeData > VSCP_LEVEL2_MAXDATA)
        return false;

    if (pEventEx->sizeData) {
        // Allocate memory for data
        if (NULL == (pEvent->pdata = new uint8_t[pEventEx->sizeData]))
            return false;
        memcpy(pEvent->pdata, pEventEx->data, pEventEx->sizeData);
    } else {
        // No data
        pEvent->pdata = NULL;
    }

    // Convert
    pEvent->crc        = pEventEx->crc;
    pEvent->obid       = pEventEx->obid;
    pEvent->year       = pEventEx->year;
    pEvent->month      = pEventEx->month;
    pEvent->day        = pEventEx->day;
    pEvent->hour       = pEventEx->hour;
    pEvent->minute     = pEventEx->minute;
    pEvent->second     = pEventEx->second;
    pEvent->timestamp  = pEventEx->timestamp;
    pEvent->head       = pEventEx->head;
    pEvent->obid       = pEventEx->obid;
    pEvent->vscp_class = pEventEx->vscp_class;
    pEvent->vscp_type  = pEventEx->vscp_type;
    pEvent->sizeData   = pEventEx->sizeData;
    memcpy(pEvent->GUID, pEventEx->GUID, 16);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// copyEvent

bool
vscp_copyEvent(vscpEvent* pEventTo, const vscpEvent* pEventFrom)
{
    // Check pointers
    if (NULL == pEventTo)
        return false;
    if (NULL == pEventFrom)
        return false;

    if (pEventFrom->sizeData > VSCP_LEVEL2_MAXDATA)
        return false;

    // Convert
    pEventTo->crc        = pEventFrom->crc;
    pEventTo->obid       = pEventFrom->obid;
    pEventTo->year       = pEventFrom->year;
    pEventTo->month      = pEventFrom->month;
    pEventTo->day        = pEventFrom->day;
    pEventTo->hour       = pEventFrom->hour;
    pEventTo->minute     = pEventFrom->minute;
    pEventTo->second     = pEventFrom->second;
    pEventTo->timestamp  = pEventFrom->timestamp;
    pEventTo->head       = pEventFrom->head;
    pEventTo->obid       = pEventFrom->obid;
    pEventTo->vscp_class = pEventFrom->vscp_class;
    pEventTo->vscp_type  = pEventFrom->vscp_type;
    pEventTo->sizeData   = pEventFrom->sizeData;

    memcpy(pEventTo->GUID, pEventFrom->GUID, 16);

    if (pEventFrom->sizeData) {

        pEventTo->pdata = new unsigned char[pEventFrom->sizeData];
        if (NULL == pEventTo->pdata) {
            return false;
        }

        memcpy(pEventTo->pdata, pEventFrom->pdata, pEventFrom->sizeData);
    } else {
        pEventTo->pdata = NULL;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// copyEventEx

bool
vscp_copyEventEx(vscpEventEx* pEventTo, const vscpEventEx* pEventFrom)
{
    // Check pointers
    if (NULL == pEventTo)
        return false;
    if (NULL == pEventFrom)
        return false;

    if (pEventFrom->sizeData > VSCP_LEVEL2_MAXDATA)
        return false;

    // Convert
    pEventTo->crc        = pEventFrom->crc;
    pEventTo->obid       = pEventFrom->obid;
    pEventTo->year       = pEventFrom->year;
    pEventTo->month      = pEventFrom->month;
    pEventTo->day        = pEventFrom->day;
    pEventTo->hour       = pEventFrom->hour;
    pEventTo->minute     = pEventFrom->minute;
    pEventTo->second     = pEventFrom->second;
    pEventTo->timestamp  = pEventFrom->timestamp;
    pEventTo->head       = pEventFrom->head;
    pEventTo->obid       = pEventFrom->obid;
    pEventTo->vscp_class = pEventFrom->vscp_class;
    pEventTo->vscp_type  = pEventFrom->vscp_type;
    pEventTo->sizeData   = pEventFrom->sizeData;

    memcpy(pEventTo->GUID, pEventFrom->GUID, 16);
    memcpy(pEventTo->data, pEventFrom->data, pEventFrom->sizeData);

    if (pEventFrom->sizeData) {
        memcpy(pEventTo->data, pEventFrom->data, pEventFrom->sizeData);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_newEvent
//

bool
vscp_newEvent(vscpEvent** ppEvent)
{
    *ppEvent = new vscpEvent;
    if (NULL == *ppEvent)
        return false;

    // No data allocated yet
    (*ppEvent)->sizeData = 0;
    (*ppEvent)->pdata    = NULL;

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// deleteVSCPevent
//

void
vscp_deleteEvent(vscpEvent* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return;

    if (NULL != pEvent->pdata) {
        delete[] pEvent->pdata;
        pEvent->pdata = NULL;
    }
}

////////////////////////////////////////////////////////////////////////////////////
// deleteVSCPevent_v2
//

void
vscp_deleteEvent_v2(vscpEvent** ppEvent)
{
    // Check pointer
    if (NULL == *ppEvent)
        return;

    vscp_deleteEvent(*ppEvent);

    // Delete the event and mark it as unused.
    delete *ppEvent;
    *ppEvent = NULL;
}

////////////////////////////////////////////////////////////////////////////////////
// deleteVSCPevent
//

void
vscp_deleteEventEx(vscpEventEx* pEventEx)
{
    delete pEventEx;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_getDateStringFromEvent
//

bool
vscp_getDateStringFromEvent(std::string& dt, const vscpEvent* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    dt.clear();

    // Return empty string if all date/time values is zero
    if (pEvent->year || pEvent->month || pEvent->day || pEvent->hour ||
        pEvent->minute || pEvent->second) {
        dt = vscp_str_format("%04d-%02d-%02dT%02d:%02d:%02dZ",
                             (int)pEvent->year,
                             (int)pEvent->month,
                             (int)pEvent->day,
                             (int)pEvent->hour,
                             (int)pEvent->minute,
                             (int)pEvent->second);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_getDateStringFromEventEx
//

bool
vscp_getDateStringFromEventEx(std::string& dt, const vscpEventEx* pEventEx)
{
    // Check pointer
    if (NULL == pEventEx)
        return false;

    dt = vscp_str_format("%04d-%02d-%02dT%02d:%02d:%02dZ",
                         (int)pEventEx->year,
                         (int)pEventEx->month,
                         (int)pEventEx->day,
                         (int)pEventEx->hour,
                         (int)pEventEx->minute,
                         (int)pEventEx->second);
    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertEventToJSON
//

bool
vscp_convertEventToJSON(std::string& strJSON, vscpEvent* pEvent)
{
    std::string strguid;
    std::string strdata;

    // Check pointer
    if (NULL == pEvent)
        return false;

    vscp_writeGuidArrayToString(strguid, pEvent->GUID); // GUID to string
    vscp_writeDataWithSizeToString(strdata,
                                   pEvent->pdata,
                                   pEvent->sizeData,
                                   false,
                                   false,
                                   true);

    std::string dt;
    vscp_getDateStringFromEvent(dt, pEvent);

    // datetime,head,obid,datetime,timestamp,class,type,guid,data,note
    strJSON = vscp_str_format(VSCP_JSON_EVENT_TEMPLATE,
                              (unsigned short int)pEvent->head,
                              (unsigned long)pEvent->obid,
                              (const char*)dt.c_str(),
                              (unsigned long)pEvent->timestamp,
                              (unsigned short int)pEvent->vscp_class,
                              (unsigned short int)pEvent->vscp_type,
                              (const char*)strguid.c_str(),
                              (const char*)strdata.c_str(),
                              "");

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertJSONToEvent
//
// {
//    "head": 2,
//    "obid"; 123,
//    "datetime": "2017-01-13T10:16:02",
//    "timestamp":50817,
//    "class": 10,
//    "type": 8,
//    "guid": "00:00:00:00:00:00:00:00:00:00:00:00:00:01:00:02",
//    "data": [1,2,3,4,5,6,7]
// }

bool
vscp_convertJSONToEvent(vscpEvent* pEvent, std::string& strJSON)
{
    std::string strguid;

    // Check pointer
    if (NULL == pEvent)
        return false;

    try {

        auto j = json::parse(strJSON);

        // Head
        if (j.find("head") != j.end()) {
            pEvent->head = j.at("head").get<uint16_t>();
        }

        // obid
        if (j.find("obid") != j.end()) {
            pEvent->obid = j.at("obid").get<uint32_t>();
        }

        // TimeStamp
        if (j.find("timestamp") != j.end()) {
            pEvent->timestamp = j.at("timestamp").get<uint32_t>();
        }

        // DateTime
        if (j.find("datetime") != j.end()) {
            std::string dtStr = j.at("datetime").get<std::string>();
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            vscp_parseISOCombined(&tm, dtStr);
            vscp_setEventDateTime(pEvent, &tm);
        }

        // VSCP class
        if (j.find("class") != j.end()) {
            pEvent->vscp_class = j.at("class").get<uint16_t>();
        }

        // VSCP type
        if (j.find("type") != j.end()) {
            pEvent->vscp_type = j.at("type").get<uint16_t>();
        }

        // GUID
        if (j.find("guid") != j.end()) {
            std::string guidStr = j.at("guid").get<std::string>();
            cguid guid;
            guid.getFromString(guidStr);
            guid.writeGUID(pEvent->GUID);
        }

        pEvent->sizeData = 0;
        if (j.find("data") != j.end()) {

            std::vector<std::uint8_t> data_array = j.at("data");

            // Check size
            if (data_array.size() > VSCP_MAX_DATA)
                return false;

            pEvent->sizeData = data_array.size();
            if (0 == pEvent->sizeData) {
                pEvent->pdata = NULL;
            } else {
                pEvent->pdata = new uint8_t[data_array.size()];
                if (NULL == pEvent->pdata)
                    return false;

                // memcpy( pEvent->pdata, &data_array[ 0 ], data_array.size() );
                // C++11 variant of above
                memcpy(pEvent->pdata, data_array.data(), data_array.size());
            }
        }

    } catch (...) {
        return false;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertEventExToJSON
//

bool
vscp_convertEventExToJSON(std::string& strJSON, vscpEventEx* pEventEx)
{
    std::string strguid;
    std::string strdata;

    // Check pointer
    if (NULL == pEventEx)
        return false;

    vscp_writeGuidArrayToString(strguid, pEventEx->GUID); // GUID to string
    vscp_writeDataWithSizeToString(strdata,
                                   pEventEx->data,
                                   pEventEx->sizeData,
                                   false,
                                   false,
                                   true);

    std::string dt;
    vscp_getDateStringFromEventEx(dt, pEventEx);

    // datetime,head,obid,datetime,timestamp,class,type,guid,data,note
    strJSON = vscp_str_format(VSCP_JSON_EVENT_TEMPLATE,
                              (unsigned short int)pEventEx->head,
                              (unsigned long)pEventEx->obid,
                              (const char*)dt.c_str(),
                              (unsigned long)pEventEx->timestamp,
                              (unsigned short int)pEventEx->vscp_class,
                              (unsigned short int)pEventEx->vscp_type,
                              (const char*)strguid.c_str(),
                              (const char*)strdata.c_str(),
                              "");

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertJSONToEvent
//
// {
//    "head": 2,
//    "obid"; 123,
//    "datetime": "2017-01-13T10:16:02",
//    "timestamp":50817,
//    "class": 10,
//    "type": 8,
//    "guid": "00:00:00:00:00:00:00:00:00:00:00:00:00:01:00:02",
//    "data": [1,2,3,4,5,6,7]
// }

bool
vscp_convertJSONToEventEx(vscpEventEx* pEventEx, std::string& strJSON)
{
    std::string strguid;

    // Check pointer
    if (NULL == pEventEx)
        return false;

    try {

        auto j = json::parse(strJSON);

        // Head
        if (j.find("head") != j.end()) {
            pEventEx->head = j.at("head").get<uint16_t>();
        }

        // obid
        if (j.find("obid") != j.end()) {
            pEventEx->obid = j.at("obid").get<uint32_t>();
        }

        // TimeStamp
        if (j.find("timestamp") != j.end()) {
            pEventEx->timestamp = j.at("timestamp").get<uint32_t>();
        }

        // DateTime
        if (j.find("datetime") != j.end()) {
            std::string dtStr = j.at("datetime").get<std::string>();
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            vscp_parseISOCombined(&tm, dtStr);
            vscp_setEventExDateTime(pEventEx, &tm);
        }

        // VSCP class
        if (j.find("class") != j.end()) {
            pEventEx->vscp_class = j.at("class").get<uint16_t>();
        }

        // VSCP type
        if (j.find("type") != j.end()) {
            pEventEx->vscp_type = j.at("type").get<uint16_t>();
        }

        // GUID
        if (j.find("guid") != j.end()) {
            std::string guidStr = j.at("guid").get<std::string>();
            cguid guid;
            guid.getFromString(guidStr);
            guid.writeGUID(pEventEx->GUID);
        }

        pEventEx->sizeData = 0;
        if (j.find("data") != j.end()) {

            std::vector<std::uint8_t> data_array = j.at("data");

            // Check size
            if (data_array.size() > VSCP_MAX_DATA)
                return false;

            pEventEx->sizeData = data_array.size();
            if (0 == pEventEx->sizeData) {
                memset(pEventEx->data, 0, sizeof(pEventEx->data));
            } else {

                // memcpy( pEvent->pdata, &data_array[ 0 ], data_array.size() );
                // C++11 variant of above
                memcpy(pEventEx->data, data_array.data(), data_array.size());
            }
        }

    } catch (...) {
        return false;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertEventToXML
//

bool
vscp_convertEventToXML(std::string& strXML, vscpEvent* pEvent)
{
    std::string strguid;
    std::string strdata;

    // Check pointer
    if (NULL == pEvent)
        return false;

    vscp_writeGuidArrayToString(strguid, pEvent->GUID); // GUID to string
    vscp_writeDataWithSizeToString(strdata,
                                   pEvent->pdata,
                                   pEvent->sizeData,
                                   false,
                                   false); // Event data to string

    std::string dt;
    vscp_getDateStringFromEvent(dt, pEvent);

    // datetime,head,obid,datetime,timestamp,class,type,guid,sizedata,data,note
    strXML = vscp_str_format(VSCP_XML_EVENT_TEMPLATE,
                             (unsigned short int)pEvent->head,
                             (unsigned long)pEvent->obid,
                             (const char*)dt.c_str(),
                             (unsigned long)pEvent->timestamp,
                             (unsigned short int)pEvent->vscp_class,
                             (unsigned short int)pEvent->vscp_type,
                             (const char*)strguid.c_str(),
                             (unsigned short int)pEvent->sizeData,
                             (const char*)strdata.c_str());

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertXMLToEvent
//
// <event
//     head = "2"
//     obid = "123"
//     datetime = "2017-01-13T10:16:02"
//     timestamp = "50817"
//     class = "10"
//     type = "8"
//     guid = "00:00:00:00:00:00:00:00:00:00:00:00:00:01:00:02"
//     data = 1,2,3,4,5,6,7"
// />

// ----------------------------------------------------------------------------

int depth_event_parser = 0;

static void
startEventXMLParser(void* data, const char* name, const char** attr)
{
    vscpEvent* pev = (vscpEvent*)data;
    if (NULL == pev)
        return;

    if ((0 == strcmp(name, "event")) && (0 == depth_event_parser)) {

        for (int i = 0; attr[i]; i += 2) {

            std::string attribute = attr[i + 1];
            if (0 == strcmp(attr[i], "head")) {
                pev->head = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "obid")) {
                pev->obid = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "datetime")) {
                struct tm tm;
                memset(&tm, 0, sizeof(tm));
                std::string dt = attribute;
                if (vscp_parseISOCombined(&tm, dt)) {
                    vscp_setEventDateTime(pev, &tm);
                }
            } else if (0 == strcmp(attr[i], "timestamp")) {
                pev->timestamp = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "class")) {
                pev->vscp_class = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "type")) {
                pev->vscp_type = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "guid")) {
                std::string strGUID = attribute;
                vscp_setEventGuidFromString(pev, strGUID);
            } else if (0 == strcmp(attr[i], "data")) {
                std::string str = attribute;
                vscp_setEventDataFromString(pev, str);
            }
        }
    }

    depth_event_parser++;
}

static void
endEventXMLParser(void* data, const char* name)
{
    depth_event_parser--;
}

bool
vscp_convertXMLToEvent(vscpEvent* pEvent, std::string& strXML)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    // Must have some XML data
    if (0 == strXML.length())
        return false;

    XML_Parser xmlParser = XML_ParserCreate("UTF-8");
    XML_SetUserData(xmlParser, pEvent);
    XML_SetElementHandler(xmlParser, startEventXMLParser, endEventXMLParser);

    int bytes_read;
    void* buf = XML_GetBuffer(xmlParser, XML_BUFF_SIZE);

    strncpy((char*)buf, strXML.c_str(), strXML.length());

    bytes_read = strXML.length();
    if (!XML_ParseBuffer(xmlParser, bytes_read, bytes_read == 0)) {
        return false;
    }

    XML_ParserFree(xmlParser);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertEventExToXML
//

bool
vscp_convertEventExToXML(std::string& strXML, vscpEventEx* pEventEx)
{
    std::string strguid;
    std::string strdata;

    // Check pointer
    if (NULL == pEventEx)
        return false;

    vscp_writeGuidArrayToString(strguid, pEventEx->GUID); // GUID to string
    vscp_writeDataWithSizeToString(strdata,
                                   pEventEx->data,
                                   pEventEx->sizeData,
                                   false,
                                   false); // Event data to string

    std::string dt;
    vscp_getDateStringFromEventEx(dt, pEventEx);

    // datetime,head,obid,datetime,timestamp,class,type,guid,sizedata,data,note
    strXML = vscp_str_format(VSCP_XML_EVENT_TEMPLATE,
                             (unsigned short int)pEventEx->head,
                             (unsigned long)pEventEx->obid,
                             (const char*)dt.c_str(),
                             (unsigned long)pEventEx->timestamp,
                             (unsigned short int)pEventEx->vscp_class,
                             (unsigned short int)pEventEx->vscp_type,
                             (const char*)strguid.c_str(),
                             (unsigned short int)pEventEx->sizeData,
                             (const char*)strdata.c_str());

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertXMLToEventEx
//
// <event
//     head = "2"
//     obid = "123"
//     datetime = "2017-01-13T10:16:02"
//     timestamp = "50817"
//     class = "10"
//     type = "8"
//     guid = "00:00:00:00:00:00:00:00:00:00:00:00:00:01:00:02"
//     data = 1,2,3,4,5,6,7"
// />

// ----------------------------------------------------------------------------

int depth_eventex_parser = 0;

static void
startEventExXMLParser(void* data, const char* name, const char** attr)
{
    vscpEventEx* pex = (vscpEventEx*)data;
    if (NULL == pex)
        return;

    if ((0 == strcmp(name, "event")) && (0 == depth_eventex_parser)) {

        for (int i = 0; attr[i]; i += 2) {

            std::string attribute = attr[i + 1];
            if (0 == strcmp(attr[i], "head")) {
                pex->head = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "obid")) {
                pex->obid = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "datetime")) {
                struct tm tm;
                memset(&tm, 0, sizeof(tm));
                std::string dt = attribute;
                if (vscp_parseISOCombined(&tm, dt)) {
                    vscp_setEventExDateTime(pex, &tm);
                }
            } else if (0 == strcmp(attr[i], "timestamp")) {
                pex->timestamp = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "class")) {
                pex->vscp_class = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "type")) {
                pex->vscp_type = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "guid")) {
                std::string strGUID = attribute;
                vscp_setEventExGuidFromString(pex, strGUID);
            } else if (0 == strcmp(attr[i], "data")) {
                std::string str = attribute;
                vscp_setEventExDataFromString(pex, str);
            }
        }
    }

    depth_eventex_parser++;
}

static void
endEventExXMLParser(void* data, const char* name)
{
    depth_eventex_parser--;
}

bool
vscp_convertXMLToEventEx(vscpEventEx* pEventEx, std::string& strXML)
{
    // Check pointer
    if (NULL == pEventEx)
        return false;

    // Must have some XML data
    if (0 == strXML.length())
        return false;

    XML_Parser xmlParser = XML_ParserCreate("UTF-8");
    XML_SetUserData(xmlParser, pEventEx);
    XML_SetElementHandler(xmlParser,
                          startEventExXMLParser,
                          endEventExXMLParser);

    int bytes_read;
    void* buf = XML_GetBuffer(xmlParser, XML_BUFF_SIZE);

    strncpy((char*)buf, strXML.c_str(), strXML.length());

    bytes_read = strXML.length();
    if (!XML_ParseBuffer(xmlParser, bytes_read, bytes_read == 0)) {
        return false;
    }

    XML_ParserFree(xmlParser);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertEventToHTML
//

bool
vscp_convertEventToHTML(std::string& strHTML, vscpEvent* pEvent)
{
    std::string strguid;
    std::string strdata;

    // Check pointer
    if (NULL == pEvent)
        return false;

    vscp_writeGuidArrayToString(strguid, pEvent->GUID); // GUID to string
    vscp_writeDataWithSizeToString(strdata,
                                   pEvent->pdata,
                                   pEvent->sizeData,
                                   false,
                                   false); // Event data to string

    std::string dt;
    vscp_getDateStringFromEvent(dt, pEvent);

    // datetime,class,type,data-count,data,guid,head,timestamp,obid,note
    strHTML = vscp_str_format(VSCP_HTML_EVENT_TEMPLATE,
                              (const char*)dt.c_str(),
                              (unsigned short int)pEvent->vscp_class,
                              (unsigned short int)pEvent->vscp_type,
                              (unsigned short int)pEvent->sizeData,
                              (const char*)strdata.c_str(),
                              (const char*)strguid.c_str(),
                              (unsigned short int)pEvent->head,
                              (const char*)dt.c_str(),
                              (unsigned long)pEvent->timestamp,
                              (unsigned long)pEvent->obid,
                              "");

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// vscp_convertEventExToHTML
//

bool
vscp_convertEventExToHTML(std::string& strHTML, vscpEventEx* pEventEx)
{
    std::string strguid;
    std::string strdata;

    // Check pointer
    if (NULL == pEventEx)
        return false;

    vscp_writeGuidArrayToString(strguid, pEventEx->GUID); // GUID to string
    vscp_writeDataWithSizeToString(strdata,
                                   pEventEx->data,
                                   pEventEx->sizeData,
                                   false,
                                   false); // Event data to string

    std::string dt;
    vscp_getDateStringFromEventEx(dt, pEventEx);

    // datetime,class,type,data-count,data,guid,head,timestamp,obid,note
    strHTML = vscp_str_format(VSCP_HTML_EVENT_TEMPLATE,
                              (const char*)dt.c_str(),
                              (unsigned short int)pEventEx->vscp_class,
                              (unsigned short int)pEventEx->vscp_type,
                              (unsigned short int)pEventEx->sizeData,
                              (const char*)strdata.c_str(),
                              (const char*)strguid.c_str(),
                              (unsigned short int)pEventEx->head,
                              (const char*)dt.c_str(),
                              (unsigned long)pEventEx->timestamp,
                              (unsigned long)pEventEx->obid,
                              "");

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_setEventDateTime
//

bool
vscp_setEventDateTime(vscpEvent* pEvent, struct tm* ptm)
{
    if (NULL == pEvent)
        return false;

    pEvent->year   = ptm->tm_year + 1900;
    pEvent->month  = ptm->tm_mon + 1;
    pEvent->day    = ptm->tm_mday;
    pEvent->hour   = ptm->tm_hour;
    pEvent->minute = ptm->tm_min;
    pEvent->second = ptm->tm_sec;

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_setEventExDateTime
//

bool
vscp_setEventExDateTime(vscpEventEx* pEventEx, struct tm* ptm)
{
    if (NULL == pEventEx)
        return false;

    pEventEx->year   = ptm->tm_year + 1900;
    pEventEx->month  = ptm->tm_mon + 1;
    pEventEx->day    = ptm->tm_mday;
    pEventEx->hour   = ptm->tm_hour;
    pEventEx->minute = ptm->tm_min;
    pEventEx->second = ptm->tm_sec;

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_setEventToNow
//

bool
vscp_setEventToNow(vscpEvent* pEvent)
{
    if (NULL == pEvent)
        return false;

    time_t rawtime;
    struct tm* ptm;

    time(&rawtime);
    ptm = gmtime(&rawtime);

    pEvent->year   = ptm->tm_year + 1900;
    pEvent->month  = ptm->tm_mon + 1;
    pEvent->day    = ptm->tm_mday;
    pEvent->hour   = ptm->tm_hour;
    pEvent->minute = ptm->tm_min;
    pEvent->second = ptm->tm_sec;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_setEventExToNow
//

bool
vscp_setEventExToNow(vscpEventEx* pEventEx)
{
    if (NULL == pEventEx)
        return false;

    time_t rawtime;
    struct tm* ptm;

    time(&rawtime);
    ptm = gmtime(&rawtime);

    pEventEx->year   = ptm->tm_year + 1900;
    pEventEx->month  = ptm->tm_mon + 1;
    pEventEx->day    = ptm->tm_mday;
    pEventEx->hour   = ptm->tm_hour;
    pEventEx->minute = ptm->tm_min;
    pEventEx->second = ptm->tm_sec;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// doLevel2Filter
//
//  filter ^ bit    mask    out
//  ============================
//        0          0       1    filter == bit, mask=don't care result = true
//        0          1       1    filter == bit, mask=valid, result = true
//        1          0       1    filter != bit, makse=don't care, result = true
//        1          1       0    filter != bit, mask=valid, result = false
//
// Mask tells *which* bits that are of interest means
// it always returns true if bit set to zero (0=don't care).
//
// Filter tells the value for valid bits. If filter bit is == 1 the bits
// must be equal to get a true filter return.
//
// So a nill mask will let everything through
//

bool
vscp_doLevel2Filter(const vscpEvent* pEvent, const vscpEventFilter* pFilter)
{
    // A NULL filter is wildcard
    if (NULL == pFilter)
        return true;

    // Must be a valid message
    if (NULL == pEvent)
        return false;

    // Test vscp_class
    if (0xffff != (uint16_t)(~(pFilter->filter_class ^ pEvent->vscp_class) |
                             ~pFilter->mask_class))
        return false;

    // Test vscp_type
    if (0xffff != (uint16_t)(~(pFilter->filter_type ^ pEvent->vscp_type) |
                             ~pFilter->mask_type))
        return false;

    // GUID
    for (int i = 0; i < 16; i++) {
        if (0xff != (uint8_t)(~(pFilter->filter_GUID[i] ^ pEvent->GUID[i]) |
                              ~pFilter->mask_GUID[i]))
            return false;
    }

    // Test priority
    if (0xff !=
        (uint8_t)(~(pFilter->filter_priority ^ vscp_getEventPriority(pEvent)) |
                  ~pFilter->mask_priority))
        return false;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_doLevel2FilterEx
//

bool
vscp_doLevel2FilterEx(const vscpEventEx* pEventEx,
                      const vscpEventFilter* pFilter)
{
    // Must be a valid client
    if (NULL == pFilter)
        return false;

    // Must be a valid message
    if (NULL == pEventEx)
        return false;

    // Test vscp_class
    if (0xffff != (uint16_t)(~(pFilter->filter_class ^ pEventEx->vscp_class) |
                             ~pFilter->mask_class))
        return false;

    // Test vscp_type
    if (0xffff != (uint16_t)(~(pFilter->filter_type ^ pEventEx->vscp_type) |
                             ~pFilter->mask_type))
        return false;

    // GUID
    for (int i = 0; i < 16; i++) {
        if (0xff != (uint8_t)(~(pFilter->filter_GUID[i] ^ pEventEx->GUID[i]) |
                              ~pFilter->mask_GUID[i]))
            return false;
    }

    // Test priority
    if (0xff != (uint8_t)(~(pFilter->filter_priority ^
                            vscp_getEventExPriority(pEventEx)) |
                          ~pFilter->mask_priority))
        return false;

    return true;
}

////////////////////////////////////////////////////////////////////////////////////
// ClearVSCPFilter
//

void
vscp_clearVSCPFilter(vscpEventFilter* pFilter)
{
    // Validate pointer
    if (NULL == pFilter)
        return;

    pFilter->filter_priority = 0x00;
    pFilter->mask_priority   = 0x00;
    pFilter->filter_class    = 0x00;
    pFilter->mask_class      = 0x00;
    pFilter->filter_type     = 0x00;
    pFilter->mask_type       = 0x00;
    memset(pFilter->filter_GUID, 0x00, 16);
    memset(pFilter->mask_GUID, 0x00, 16);
}

//////////////////////////////////////////////////////////////////////////////
// vscp_copyVSCPFilter
//

void
vscp_copyVSCPFilter(vscpEventFilter* pToFilter,
                    const vscpEventFilter* pFromFilter)
{
    // Validate pointers
    if (NULL == pToFilter)
        return;
    if (NULL == pFromFilter)
        return;

    pToFilter->filter_priority = pFromFilter->filter_priority;
    pToFilter->mask_priority   = pFromFilter->mask_priority;
    pToFilter->filter_class    = pFromFilter->filter_class;
    pToFilter->mask_class      = pFromFilter->mask_class;
    pToFilter->filter_type     = pFromFilter->filter_type;
    pToFilter->mask_type       = pFromFilter->mask_type;
    memcpy(pToFilter->filter_GUID, pFromFilter->filter_GUID, 16);
    memcpy(pToFilter->mask_GUID, pFromFilter->mask_GUID, 16);
}

//////////////////////////////////////////////////////////////////////////////
// readFilterFromString
//

bool
vscp_readFilterFromString(vscpEventFilter* pFilter,
                          const std::string& strFilter)
{
    std::deque<std::string> tokens;

    // Check pointer
    if (NULL == pFilter)
        return false;

    pFilter->filter_priority = 0;
    pFilter->filter_class    = 0;
    pFilter->filter_type     = 0;
    memset(pFilter->filter_GUID, 0, 16);

    vscp_split(tokens, strFilter, ",");

    // Get filter priority
    if (!tokens.empty()) {
        pFilter->filter_priority = vscp_readStringValue(tokens.front());
        tokens.pop_front();
    } else {
        return true;
    }

    // Get filter class
    if (!tokens.empty()) {
        pFilter->filter_class = vscp_readStringValue(tokens.front());
        tokens.pop_front();
    } else {
        return true;
    }

    // Get filter type
    if (!tokens.empty()) {
        pFilter->filter_type = vscp_readStringValue(tokens.front());
        tokens.pop_front();
    } else {
        return true;
    }

    // Get filter GUID
    if (!tokens.empty()) {
        vscp_getGuidFromStringToArray(pFilter->filter_GUID, tokens.front());
        tokens.pop_front();
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_writeFilterToString
//

bool
vscp_writeFilterToString(std::string& strFilter, const vscpEventFilter* pFilter)
{
    cguid guid;

    // Check pointer
    if (NULL == pFilter)
        return false;

    guid.getFromArray(pFilter->filter_GUID);

    strFilter = vscp_str_format("%d,%d,%d,%s",
                                pFilter->filter_priority,
                                pFilter->filter_class,
                                pFilter->filter_type,
                                guid.getAsString().c_str());

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// readMaskFromString
//

bool
vscp_readMaskFromString(vscpEventFilter* pFilter, const std::string& strMask)
{
    std::deque<std::string> tokens;

    // Check pointer
    if (NULL == pFilter)
        return false;

    pFilter->mask_priority = 0;
    pFilter->mask_class    = 0;
    pFilter->mask_type     = 0;
    memset(pFilter->mask_GUID, 0, 16);

    vscp_split(tokens, strMask, ",");

    // Get mask priority
    if (!tokens.empty()) {
        pFilter->mask_priority = vscp_readStringValue(tokens.front());
        tokens.pop_front();
    } else {
        return true;
    }

    // Get mask class
    if (!tokens.empty()) {
        pFilter->mask_class = vscp_readStringValue(tokens.front());
        tokens.pop_front();
    } else {
        return true;
    }

    // Get mask type
    if (!tokens.empty()) {
        pFilter->mask_type = vscp_readStringValue(tokens.front());
        tokens.pop_front();
    } else {
        return true;
    }

    // Get mask GUID
    if (!tokens.empty()) {
        vscp_getGuidFromStringToArray(pFilter->mask_GUID, tokens.front());
        tokens.pop_front();
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_writeMaskToString
//

bool
vscp_writeMaskToString(std::string& strFilter, const vscpEventFilter* pFilter)
{
    cguid guid;

    // Check pointer
    if (NULL == pFilter)
        return false;

    guid.getFromArray(pFilter->mask_GUID);

    strFilter = vscp_str_format("%d,%d,%d,%s",
                                pFilter->mask_priority,
                                pFilter->mask_class,
                                pFilter->mask_type,
                                guid.getAsString().c_str());

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_readFilterMaskFromString
//

bool
vscp_readFilterMaskFromString(vscpEventFilter* pFilter,
                              const std::string& strFilterMask)
{
    std::string strTok;

    // Check pointer
    if (NULL == pFilter)
        return false;

    // Clear filter and mask
    vscp_clearVSCPFilter(pFilter);

    std::deque<std::string> tokens;
    vscp_split(tokens, strFilterMask, ",");

    // Get filter priority
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        pFilter->filter_priority = vscp_readStringValue(strTok);
    } else {
        return true;
    }

    // Get filter class
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        pFilter->filter_class = vscp_readStringValue(strTok);
    } else {
        return true;
    }

    // Get filter type
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        pFilter->filter_type = vscp_readStringValue(strTok);
    } else {
        return true;
    }

    // Get filter GUID
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        vscp_getGuidFromStringToArray(pFilter->filter_GUID, strTok);
    }

    // Get mask priority
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        pFilter->mask_priority = vscp_readStringValue(strTok);
    } else {
        return true;
    }

    // Get mask class
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        pFilter->mask_class = vscp_readStringValue(strTok);
    } else {
        return true;
    }

    // Get mask type
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        pFilter->mask_type = vscp_readStringValue(strTok);
    } else {
        return true;
    }

    // Get mask GUID
    if (!tokens.empty()) {
        strTok = tokens.front();
        tokens.pop_front();
        vscp_getGuidFromStringToArray(pFilter->mask_GUID, strTok);
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_readFilterMaskFromXML
//
// <filter
//      mask_priority = "n"
//      mask_class = "n"
//      mask_type = "n"
//      mask_guid = "xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx"
//      filter_priority = "n"
//      filter_class = "n"
//      filter_type = "n"
//      filter_guid = "xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx:xx""
// />"
//

int depth_filtermask_parser = 0;

static void
startFilterMaskXMLParser(void* data, const char* name, const char** attr)
{
    vscpEventFilter* pFilter = (vscpEventFilter*)data;
    if (NULL == pFilter)
        return;

    if ((0 == strcmp(name, "filter")) && (0 == depth_filtermask_parser)) {

        for (int i = 0; attr[i]; i += 2) {

            std::string attribute = attr[i + 1];
            if (0 == strcmp(attr[i], "mask_priority")) {
                pFilter->mask_priority = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "mask_class")) {
                pFilter->mask_class = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "mask_type")) {
                pFilter->mask_type = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "mask_guid")) {
                std::string str = attribute;
                vscp_getGuidFromStringToArray(pFilter->mask_GUID, str);
            } else if (0 == strcmp(attr[i], "filter_priority")) {
                pFilter->filter_priority = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "filter_class")) {
                pFilter->filter_class = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "filter_type")) {
                pFilter->filter_type = vscp_readStringValue(attribute);
            } else if (0 == strcmp(attr[i], "filter_guid")) {
                std::string str = attribute;
                vscp_getGuidFromStringToArray(pFilter->filter_GUID, str);
            }
        }
    }

    depth_filtermask_parser++;
}

static void
endFilterMaskXMLParser(void* data, const char* name)
{
    depth_filtermask_parser--;
}

// ----------------------------------------------------------------------------

bool
vscp_readFilterMaskFromXML(vscpEventFilter* pFilter,
                           const std::string& strFilter)
{
    // Check pointer
    if (NULL == pFilter)
        return false;

    // Must be some XML to parse
    if (0 == strFilter.length())
        return false;

    XML_Parser xmlParser = XML_ParserCreate("UTF-8");
    XML_SetUserData(xmlParser, pFilter);
    XML_SetElementHandler(xmlParser,
                          startFilterMaskXMLParser,
                          endFilterMaskXMLParser);

    int bytes_read;
    void* buf = XML_GetBuffer(xmlParser, XML_BUFF_SIZE);

    strncpy((char*)buf, strFilter.c_str(), strFilter.length());

    bytes_read = strFilter.length();
    if (!XML_ParseBuffer(xmlParser, bytes_read, bytes_read == 0)) {
        return false;
    }

    XML_ParserFree(xmlParser);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_writeFilterMaskToXML
//

bool
vscp_writeFilterMaskToXML(std::string& strFilter, vscpEventFilter* pFilter)
{
    std::string strmaskguid;
    std::string strfilterguid;

    // Check pointer
    if (NULL == pFilter)
        return false;

    vscp_writeGuidArrayToString(strmaskguid, pFilter->mask_GUID);
    vscp_writeGuidArrayToString(strfilterguid, pFilter->filter_GUID);

    strFilter = vscp_str_format(VSCP_XML_FILTER_TEMPLATE,
                                (int)pFilter->mask_priority,
                                (int)pFilter->mask_class,
                                (int)pFilter->mask_type,
                                (const char*)strmaskguid.c_str(),
                                (int)pFilter->filter_priority,
                                (int)pFilter->filter_class,
                                (int)pFilter->filter_type,
                                (const char*)strfilterguid.c_str());
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_readFilterMaskFromJSON
//

bool
vscp_readFilterMaskFromJSON(vscpEventFilter* pFilter,
                            const std::string& strFilter)
{
    std::string strguid;

    // Check pointer
    if (NULL == pFilter)
        return false;

    try {

        auto j = json::parse(strFilter);

        // mask priority
        if (j.find("mask_priority") != j.end()) {
            pFilter->mask_priority = j.at("mask_priority").get<uint8_t>();
        }

        // mask_class
        if (j.find("mask_class") != j.end()) {
            pFilter->mask_class = j.at("mask_class").get<uint16_t>();
        }

        // mask_type
        if (j.find("mask_type") != j.end()) {
            pFilter->mask_type = j.at("mask_type").get<uint16_t>();
        }

        // mask GUID
        if (j.find("mask_guid") != j.end()) {
            std::string guidStr = j.at("mask_guid").get<std::string>();
            cguid guid;
            guid.getFromString(guidStr);
            guid.writeGUID(pFilter->mask_GUID);
        }

        // filter priority
        if (j.find("filter_priority") != j.end()) {
            pFilter->filter_priority = j.at("filter_priority").get<uint8_t>();
        }

        // filter_class
        if (j.find("filter_class") != j.end()) {
            pFilter->filter_class = j.at("filter_class").get<uint16_t>();
        }

        // filter_type
        if (j.find("filter_type") != j.end()) {
            pFilter->filter_type = j.at("filter_type").get<uint16_t>();
        }

        // filter GUID
        if (j.find("filter_guid") != j.end()) {
            std::string guidStr = j.at("filter_guid").get<std::string>();
            cguid guid;
            guid.getFromString(guidStr);
            guid.writeGUID(pFilter->filter_GUID);
        }

    } catch (...) {
        return false;
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_writeFilterMaskToJSON
//

bool
vscp_writeFilterMaskToJSON(vscpEventFilter* pFilter, std::string& strFilter)
{
    std::string strmaskguid;
    std::string strfilterguid;

    // Check pointer
    if (NULL == pFilter)
        return false;

    vscp_writeGuidArrayToString(strmaskguid, pFilter->mask_GUID);
    vscp_writeGuidArrayToString(strfilterguid, pFilter->filter_GUID);

    strFilter = vscp_str_format(VSCP_JSON_FILTER_TEMPLATE,
                                (int)pFilter->mask_priority,
                                (int)pFilter->mask_class,
                                (int)pFilter->mask_type,
                                (const char*)strmaskguid.c_str(),
                                (int)pFilter->filter_priority,
                                (int)pFilter->filter_class,
                                (int)pFilter->filter_type,
                                (const char*)strfilterguid.c_str());

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// convertCanalToEvent
//

bool
vscp_convertCanalToEvent(vscpEvent* pvscpEvent,
                         const canalMsg* pcanalMsg,
                         unsigned char* pGUID)
{
    // Must be valid pointers
    if (NULL == pGUID)
        return false;
    if (NULL == pcanalMsg)
        return false;
    if (NULL == pvscpEvent)
        return false;

    // Copy in i/f GUID
    memcpy(pvscpEvent->GUID, pGUID, 16);

    pvscpEvent->head = 0;

    if (pcanalMsg->sizeData > 8) {
        return false;
    }

    if (pcanalMsg->sizeData > 0) {

        // Allocate storage for data
        pvscpEvent->pdata = new uint8_t[pcanalMsg->sizeData];

        if (NULL != pvscpEvent->pdata) {
            // Assign size (max 8 bytes it's CAN... )
            pvscpEvent->sizeData = pcanalMsg->sizeData;
            memcpy(pvscpEvent->pdata, pcanalMsg->data, pcanalMsg->sizeData);
        } else {
            pvscpEvent->sizeData = 0;
        }
    } else {
        pvscpEvent->pdata    = NULL;
        pvscpEvent->sizeData = 0;
    }

    // Build ID
    pvscpEvent->head = vscp_getHeadFromCANALid(pcanalMsg->id);
    if (pcanalMsg->id & 0x02000000)
        pvscpEvent->head |= VSCP_HEADER_HARD_CODED;
    pvscpEvent->vscp_class = vscp_getVscpClassFromCANALid(pcanalMsg->id);
    pvscpEvent->vscp_type  = vscp_getVscpTypeFromCANALid(pcanalMsg->id);

    // Timestamp
    vscp_setEventDateTimeBlockToNow(pvscpEvent);
    pvscpEvent->timestamp = pcanalMsg->timestamp;

    // Date/time block
    vscp_setEventToNow(pvscpEvent);

    // Set nickname id
    pvscpEvent->GUID[15] = (unsigned char)(0xff & pcanalMsg->id);

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// convertCanalToEvent
//

bool
vscp_convertCanalToEventEx(vscpEventEx* pvscpEventEx,
                           const canalMsg* pcanalMsg,
                           unsigned char* pGUID)
{
    vscpEvent* pEvent = new vscpEvent;
    bool rv           = vscp_convertCanalToEvent(pEvent, pcanalMsg, pGUID);

    if (rv) {
        vscp_convertEventToEventEx(pvscpEventEx, pEvent);
        vscp_deleteEvent(pEvent);
    }

    return rv;
}

//////////////////////////////////////////////////////////////////////////////
// convertEventToCanal
//

bool
vscp_convertEventToCanal(canalMsg* pcanalMsg, const vscpEvent* pvscpEvent)
{
    unsigned char nodeid = 0;
    short sizeData       = 0;
    uint16_t vscp_class  = 0;

    if (NULL == pcanalMsg)
        return false;
    if (NULL == pvscpEvent)
        return false;

    sizeData   = pvscpEvent->sizeData;
    vscp_class = pvscpEvent->vscp_class;
    nodeid     = pvscpEvent->GUID[15];

    pcanalMsg->obid  = pvscpEvent->obid;
    pcanalMsg->flags = 0;

    // Level II events with class == 512 is recognized by the daemon and
    // sent to the correct interface as Level I events if the interface
    // is addressed by the client.
    if ((512 == pvscpEvent->vscp_class) && (pvscpEvent->sizeData >= 16)) {

        nodeid = pvscpEvent->pdata[15]; // Save the nodeid

        // We must translate the data part of the event to standard format
        sizeData = pvscpEvent->sizeData - 16;
        memcpy(pvscpEvent->pdata, pvscpEvent->pdata + 16, sizeData);

        vscp_class = pvscpEvent->vscp_class - 512;
    }

    // Always extended
    pcanalMsg->flags = CANAL_IDFLAG_EXTENDED;

    if (sizeData <= 8) {
        pcanalMsg->sizeData = (unsigned char)sizeData;
    } else {
        pcanalMsg->sizeData = 8;
    }

    unsigned char priority = ((pvscpEvent->head & VSCP_MASK_PRIORITY) >> 5);

    // unsigned long t1 = (unsigned long)priority << 20;
    // unsigned long t2 = (unsigned long)pvscpMsg->vscp_class << 16;
    // unsigned long t3 = (unsigned long)pvscpMsg->vscp_type << 8;
    pcanalMsg->id = ((unsigned long)priority << 26) |
                    ((unsigned long)vscp_class << 16) |
                    ((unsigned long)pvscpEvent->vscp_type << 8) |
                    nodeid; // Normally we are the host of hosts
    // but for class=512 events nodeid
    // is present in GUID LSB

    if (pvscpEvent->head & VSCP_HEADER_HARD_CODED) {
        pcanalMsg->id |= VSCP_CAN_ID_HARD_CODED;
    }

    if (NULL != pvscpEvent->pdata) {
        memcpy(pcanalMsg->data, pvscpEvent->pdata, pcanalMsg->sizeData);
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// convertEventExToCanal
//

bool
vscp_convertEventExToCanal(canalMsg* pcanalMsg, const vscpEventEx* pvscpEventEx)
{
    bool rv;

    if (NULL == pcanalMsg)
        return false;
    if (NULL == pvscpEventEx)
        return false;

    vscpEvent* pEvent = new vscpEvent();
    if (NULL == pEvent)
        return false;

    if (!vscp_convertEventExToEvent(pEvent, pvscpEventEx)) {
        vscp_deleteEvent(pEvent);
        return false;
    }

    rv = vscp_convertEventToCanal(pcanalMsg, pEvent);

    vscp_deleteEvent(pEvent);

    return rv;
}

//////////////////////////////////////////////////////////////////////////////
// writeDataToString
//

bool
vscp_writeDataToString(std::string& str,
                       const vscpEvent* pEvent,
                       bool bUseHtmlBreak,
                       bool bBreak)
{
    std::string wrk, strBreak;

    // Check pointers
    if (NULL == pEvent->pdata)
        return false;

    str.clear();

    if (bUseHtmlBreak) {
        strBreak = "<br>";
    } else {
        strBreak = "\r\n";
    }

    for (int i = 0; i < pEvent->sizeData; i++) {

        wrk = vscp_str_format("0x%02X", pEvent->pdata[i]);

        if (i < (pEvent->sizeData - 1)) {
            wrk += ",";
        }

        if (bBreak) {
            if (!((i + 1) % 8))
                wrk += strBreak;
        }
        str += wrk;
    }

    return true;
}

//////////////////////////////////////g////////////////////////////////////////
// writeDataWithSizeToString
//

bool
vscp_writeDataWithSizeToString(std::string& str,
                               const unsigned char* pData,
                               const uint16_t sizeData,
                               bool bUseHtmlBreak,
                               bool bBreak,
                               bool bDecimal)
{
    std::string wrk, strBreak;

    // Check pointers
    if (NULL == pData)
        return false;

    str.clear();

    if (bUseHtmlBreak) {
        strBreak = "<br>";
    } else {
        strBreak = "\r\n";
    }

    for (int i = 0; i < sizeData; i++) {

        if (bDecimal) {
            wrk = vscp_str_format("%d", pData[i]);
        } else {
            wrk = vscp_str_format("0x%02X", pData[i]);
        }

        if (i < (sizeData - 1)) {
            wrk += ",";
        }

        if (bBreak) {
            if (!((i + 1) % 8))
                wrk += strBreak;
        }
        str += wrk;
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// setEventDataFromString
//

bool
vscp_setEventDataFromString(vscpEvent* pEvent, const std::string& str)
{
    std::deque<std::string> tokens;

    // Check pointers
    if (NULL == pEvent)
        return false;

    vscp_split(tokens, str, ",");

    uint8_t data[VSCP_MAX_DATA];
    pEvent->sizeData = 0;

    while (!tokens.empty()) {
        std::string token = tokens.front();
        tokens.pop_front();
        data[pEvent->sizeData++] = vscp_readStringValue(token);
        if (pEvent->sizeData >= VSCP_MAX_DATA)
            break;
    }

    if (pEvent->sizeData > 0) {
        pEvent->pdata = new uint8_t[pEvent->sizeData];
        if (NULL != pEvent->pdata) {
            memcpy(pEvent->pdata, &data, pEvent->sizeData);
        }
    } else {
        pEvent->pdata = NULL;
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// vscp_setEventExDataFromString
//

bool
vscp_setEventExDataFromString(vscpEventEx* pEventEx, const std::string& str)
{
    std::deque<std::string> tokens;

    // Check pointers
    if (NULL == pEventEx)
        return false;

    vscp_split(tokens, str, ",");

    pEventEx->sizeData = 0;
    while (!tokens.empty()) {
        std::string token = tokens.front();
        tokens.pop_front();
        pEventEx->data[pEventEx->sizeData] = vscp_readStringValue(token);
        pEventEx->sizeData++;
        if (pEventEx->sizeData >= VSCP_MAX_DATA)
            break;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// setDataArrayFromString
//

bool
vscp_setDataArrayFromString(uint8_t* pData,
                            uint16_t* psizeData,
                            const std::string& str)
{
    // Check pointers
    if (NULL == pData)
        return false;
    if (NULL == psizeData)
        return false;

    *psizeData = 0;
    std::deque<std::string> tokens;
    vscp_split(tokens, str, ",");

    while (!tokens.empty()) {
        std::string token = tokens.front();
        tokens.pop_front();
        pData[*psizeData] = vscp_readStringValue(token);
        (*psizeData)++;
        if (*psizeData >= VSCP_MAX_DATA)
            break;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// makeTimeStamp
//
// Linux: Clock
// Windows: GetTickCount()
// http://www.decompile.com/cpp/faq/windows_timer_api.htm
// https://docs.microsoft.com/en-us/windows/desktop/sysinfo/acquiring-high-resolution-time-stamps
//

unsigned long
vscp_makeTimeStamp(void)
{
    uint32_t us; // Microseconds
    time_t s;    // Seconds
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    s  = spec.tv_sec;
    us = round(s * 1000 + spec.tv_nsec / 1000); // Convert to microseconds
    return us;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getMsTimeStamp
//

uint32_t
vscp_getMsTimeStamp(void)
{
#ifdef WIN32
    return GetTickCount();
#else
    // --- marked obsolete ---
    // timeval curTime;
    // gettimeofday( &curTime, NULL );
    // return  ( 1000 * curTime.tv_sec + curTime.tv_usec / 1000 );
    // --- marked obsolite ---
    uint32_t ms; // Milliseconds
    time_t s;    // Seconds
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    s  = spec.tv_sec;
    ms = round(s * 1000 + spec.tv_nsec / 1.0e6); // Convert to milliseconds
    return ms;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// vscp_setEventDateTimeBlockToNow
//

bool
vscp_setEventDateTimeBlockToNow(vscpEvent* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    time_t rawtime;
    struct tm* ptm;

    time(&rawtime);
    ptm = gmtime(&rawtime);

    pEvent->year   = ptm->tm_year + 1900;
    pEvent->month  = ptm->tm_mon + 1;
    pEvent->day    = ptm->tm_mday;
    pEvent->hour   = ptm->tm_hour;
    pEvent->minute = ptm->tm_min;
    pEvent->second = ptm->tm_sec;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_setEventExDateTimeBlockToNow
//

bool
vscp_setEventExDateTimeBlockToNow(vscpEventEx* pEventEx)
{
    // Check pointer
    if (NULL == pEventEx)
        return false;

    time_t rawtime;
    struct tm* ptm;

    time(&rawtime);
    ptm = gmtime(&rawtime);

    pEventEx->year   = ptm->tm_year + 1900;
    pEventEx->month  = ptm->tm_mon + 1;
    pEventEx->day    = ptm->tm_mday;
    pEventEx->hour   = ptm->tm_hour;
    pEventEx->minute = ptm->tm_min;
    pEventEx->second = ptm->tm_sec;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// convertEventToString
//
// head,class,type,obid,datetime,timestamp,GUID,data1,data2,data3....
//

bool
vscp_convertEventToString(std::string& str, const vscpEvent* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    std::string dt;
    vscp_getDateStringFromEvent(dt, pEvent);

    // head,class,type,obid,datetime,timestamp
    str = vscp_str_format("%hu,%hu,%hu,%lu,%s,%lu,",
                          (unsigned short)pEvent->head,
                          (unsigned short)pEvent->vscp_class,
                          (unsigned short)pEvent->vscp_type,
                          (unsigned long)pEvent->obid,
                          (const char*)dt.c_str(),
                          (unsigned long)pEvent->timestamp);

    std::string strGUID;
    vscp_writeGuidToString(strGUID, pEvent);
    str += strGUID;
    if (pEvent->sizeData) {
        str += ",";

        std::string strData;
        vscp_writeDataToString(strData, pEvent);
        str += strData;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getEventAsString
//

std::string
vscp_getEventAsString(const vscpEvent* pEvent)
{
    std::string str;
    vscp_convertEventToString(str, pEvent);
    return str;
}

///////////////////////////////////////////////////////////////////////////////
// convertEventExToString
//
// head,class,type,obid,datetime,timestamp,GUID,data1,data2,data3....
//

bool
vscp_convertEventExToString(std::string& str, const vscpEventEx* pEventEx)
{
    vscpEvent event;
    event.pdata = NULL;

    // Check pointer
    if (NULL == pEventEx)
        return false;

    vscp_convertEventExToEvent(&event, pEventEx);
    vscp_convertEventToString(str, &event);
    if (NULL != event.pdata)
        delete event.pdata;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getEventExAsString
//

std::string
vscp_getEventExAsString(const vscpEventEx* pEventEx)
{
    std::string str;
    vscp_convertEventExToString(str, pEventEx);
    return str;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_convertStringToEvent
//
// Format:
//      head,class,type,obid,datetime,timestamp,GUID,data1,data2,data3....
//

bool
vscp_convertStringToEvent(vscpEvent* pEvent, const std::string& strEvent)
{
    // Check pointer
    if (NULL == pEvent) {
        return false;
    }

    std::string str = strEvent;

    std::deque<std::string> tokens;
    vscp_split(tokens, str, ",");

    // Get head
    if (!tokens.empty()) {
        str = tokens.front();
        tokens.pop_front();
        pEvent->head = vscp_readStringValue(str);
    } else {
        return false;
    }

    // Get Class
    if (!tokens.empty()) {
        str = tokens.front();
        tokens.pop_front();
        pEvent->vscp_class = vscp_readStringValue(str);
    } else {
        return false;
    }

    // Get Type
    if (!tokens.empty()) {
        str = tokens.front();
        tokens.pop_front();
        pEvent->vscp_type = vscp_readStringValue(str);
    } else {
        return false;
    }

    // Get OBID  -  Kept here to be compatible with receive
    if (!tokens.empty()) {
        str = tokens.front();
        tokens.pop_front();
        pEvent->obid = vscp_readStringValue(str);
    } else {
        return false;
    }

    // Get datetime
    if (!tokens.empty()) {
        str = tokens.front();
        tokens.pop_front();
        vscp_trim(str);
        if (str.length()) {
            // Parse and set time
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            vscp_parseISOCombined(&tm, str);
            pEvent->year   = tm.tm_year + 1900;
            pEvent->month  = tm.tm_mon;
            pEvent->day    = tm.tm_mday;
            pEvent->hour   = tm.tm_hour;
            pEvent->minute = tm.tm_min;
            pEvent->second = tm.tm_sec;
        } else {
            // Set to now
            vscp_setEventDateTimeBlockToNow(pEvent);
        }
    }

    // Get Timestamp
    if (!tokens.empty()) {
        str = tokens.front();
        tokens.pop_front();
        pEvent->timestamp = vscp_readStringValue(str);
        if (!pEvent->timestamp) {
            pEvent->timestamp = vscp_makeTimeStamp();
        }
    } else {
        return false;
    }

    // Get GUID
    std::string strGUID;
    if (!tokens.empty()) {
        str = tokens.front();
        tokens.pop_front();
        vscp_setEventGuidFromString(pEvent, strGUID);
    } else {
        return false;
    }

    // Handle data
    pEvent->sizeData = 0;

    char data[512];
    while (!tokens.empty()) {
        str = tokens.front();
        tokens.pop_front();
        data[pEvent->sizeData] = vscp_readStringValue(str);
        pEvent->sizeData++;
    }

    // OK add in the data
    if (pEvent->sizeData) {
        uint8_t* pData = new uint8_t[pEvent->sizeData];
        if (NULL != pData) {
            memcpy(pData, data, pEvent->sizeData);
            pEvent->pdata = pData;
        } else {
            return false;
        }
    } else {
        pEvent->pdata = NULL;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_convertStringToEventEx
//
// Format:
//      head,class,type,obid,timestamp,GUID,data1,data2,data3....
//

bool
vscp_convertStringToEventEx(vscpEventEx* pEventEx, const std::string& strEvent)
{
    bool rv;
    vscpEvent event;

    // Parse the string data
    rv = vscp_convertStringToEvent(&event, strEvent);
    vscp_convertEventToEventEx(pEventEx, &event);

    // Remove possible data
    if (event.sizeData)
        delete[] event.pdata;

    return rv;
}

///////////////////////////////////////////////////////////////////////////////
// makeHtml
//
//

void
vscp_makeHtml(std::string& str)
{
    std::string strOriginal = str;

    str.clear();
    for (uint32_t i = 0; i < strOriginal.length(); i++) {
        if (0x0a == (unsigned char)strOriginal.at(i)) {
            str += "<br>";
        } else if (' ' == (unsigned char)strOriginal.at(i)) {
            str += "&nbsp;";
        } else if ('<' == (unsigned char)strOriginal.at(i)) {
            str += "&lt;";
        } else if ('>' == (unsigned char)strOriginal.at(i)) {
            str += "&gt;";
        } else if ('&' == (unsigned char)strOriginal.at(i)) {
            str += "&amp;";
        } else if ('"' == (unsigned char)strOriginal.at(i)) {
            str += "&quot;";
        } else if ('\'' == (unsigned char)strOriginal.at(i)) {
            str += "&apos;";
        } else {
            str += strOriginal.at(i);
        }
    }
}

//////////////////////////////////////////////////////////////////////////////
// getDeviceHtmlStatusInfo
//

std::string&
vscp_getDeviceHtmlStatusInfo(const uint8_t* registers, CMDF* pmdf)
{
    static std::string strHTML;
    std::string str;

    strHTML = "<html><body>";
    strHTML += "<h4>Clear text node data</h4>";
    strHTML += "<font color=\"#009900\">";

    strHTML += "nodeid = ";
    str = vscp_str_format("%d", registers[0x91]);
    strHTML += str;
    strHTML += "<br>";

    strHTML += "GUID = ";
    vscp_writeGuidArrayToString(str, registers + 0xd0);
    strHTML += str;
    strHTML += "<br>";

    strHTML += "MDF URL = ";
    char url[33];
    memset(url, 0, sizeof(url));
    memcpy(url, registers + 0xe0, 32);
    str = std::string(url);
    strHTML += str;
    strHTML += "<br>";

    strHTML += "Alarm: ";
    if (registers[0x80]) {
        strHTML += "Yes";
    } else {
        strHTML += "No";
    }
    strHTML += "<br>";

    strHTML += "Node Control Flags: ";
    if (registers[0x83] & 0x10) {
        strHTML += "[Register Write Protect] ";
    } else {
        strHTML += "[Register Read/Write] ";
    }
    switch ((registers[0x83] & 0xC0) >> 6) {
        case 1:
            strHTML += " [Initialized] ";
            break;
        default:
            strHTML += " [Uninitialized] ";
            break;
    }
    strHTML += "<br>";

    strHTML += "Firmware VSCP conformance : ";
    strHTML += vscp_str_format("%d.%d", registers[0x81], registers[0x82]);
    strHTML += "<br>";

    strHTML += "User ID: ";
    strHTML += vscp_str_format("%d.%d.%d.%d.%d",
                               registers[0x84],
                               registers[0x85],
                               registers[0x86],
                               registers[0x87],
                               registers[0x88]);
    strHTML += "<br>";

    strHTML += "Manufacturer device ID: ";
    strHTML += vscp_str_format("%d.%d.%d.%d",
                               registers[0x89],
                               registers[0x8A],
                               registers[0x8B],
                               registers[0x8C]);
    strHTML += "<br>";

    strHTML += "Manufacturer sub device ID: ";
    strHTML += vscp_str_format("%d.%d.%d.%d",
                               registers[0x8d],
                               registers[0x8e],
                               registers[0x8f],
                               registers[0x90]);
    strHTML += "<br>";

    strHTML += "Page select: ";
    strHTML += vscp_str_format("%d (MSB=%d LSB=%d)",
                               registers[0x92] * 256 + registers[0x93],
                               registers[0x92],
                               registers[0x93]);
    strHTML += "<br>";

    strHTML += "Firmware version: ";
    strHTML += vscp_str_format("%d.%d.%d",
                               registers[0x94],
                               registers[0x95],
                               registers[0x96]);
    strHTML += "<br>";

    strHTML += "Boot loader algorithm: ";
    strHTML += vscp_str_format("%d - ", registers[0x97]);
    switch (registers[0x97]) {

        case 0x00:
            strHTML += "VSCP universal algorithm 0";
            break;

        case 0x01:
            strHTML += "Microchip PIC algorithm 0";
            break;

        case 0x10:
            strHTML += "Atmel AVR algorithm 0";
            break;

        case 0x20:
            strHTML += "NXP ARM algorithm 0";
            break;

        case 0x30:
            strHTML += "ST ARM algorithm 0";
            break;

        default:
            strHTML += "Unknown algorithm.";
            break;
    }

    strHTML += "<br>";

    strHTML += "Buffer size: ";
    strHTML += vscp_str_format("%d bytes. ", registers[0x98]);
    if (!registers[0x98])
        strHTML += " ( == default size (8 or 487 bytes) )";
    strHTML += "<br>";

    strHTML += "Number of register pages: ";
    strHTML += vscp_str_format("%d", registers[0x99]);
    if (registers[0x99] > 22) {
        strHTML += " (Note: VSCP Works display max 22 pages.) ";
    }
    strHTML += "<br>";

    // Decision matrix info.
    if (NULL != pmdf) {

        unsigned char data[8];
        memset(data, 0, 8);

        strHTML += "Decison Matrix: Rows=";
        strHTML += vscp_str_format("%d ", pmdf->m_dmInfo.m_nRowCount);
        strHTML += " Offset=";
        strHTML += vscp_str_format("%d ", pmdf->m_dmInfo.m_nStartOffset);
        strHTML += " Page start=";
        strHTML += vscp_str_format("%d ", pmdf->m_dmInfo.m_nStartPage);
        strHTML += " Row Size=";
        strHTML += vscp_str_format("%d ", pmdf->m_dmInfo.m_nRowSize);
        strHTML += " Level=";
        strHTML += vscp_str_format("%d ", pmdf->m_dmInfo.m_nLevel);
        strHTML += " # actions define =";
        strHTML += vscp_str_format("%d ", pmdf->m_dmInfo.m_list_action.size());
        strHTML += "<br>";
    } else {
        strHTML += "No Decision Matrix is available on this device.";
        strHTML += "<br>";
    }

    if (NULL != pmdf) {

        // MDF Info
        strHTML += "<h1>MDF Information</h1>";

        strHTML += "<font color=\"#009900\">";

        // Manufacturer data
        strHTML += "<b>Module name :</b> ";
        strHTML += pmdf->m_strModule_Name;
        strHTML += "<br>";

        strHTML += "<b>Module model:</b> ";
        strHTML += pmdf->m_strModule_Model;
        strHTML += "<br>";

        strHTML += "<b>Module version:</b> ";
        strHTML += pmdf->m_strModule_Version;
        strHTML += "<br>";

        strHTML += "<b>Module last change:</b> ";
        strHTML += pmdf->m_changeDate;
        strHTML += "<br>";

        strHTML += "<b>Module description:</b> ";
        strHTML += pmdf->m_strModule_Description;
        strHTML += "<br>";

        strHTML += "<b>Module URL</b> : ";
        strHTML += "<a href=\"";
        strHTML += pmdf->m_strModule_InfoURL;
        strHTML += "\">";
        strHTML += pmdf->m_strModule_InfoURL;
        strHTML += "</a>";
        strHTML += "<br>";

        std::deque<CMDF_Manufacturer*>::iterator iter;
        for (iter = pmdf->m_list_manufacturer.begin();
             iter != pmdf->m_list_manufacturer.end();
             ++iter) {

            strHTML += "<hr><br>";

            CMDF_Manufacturer* manufacturer = *iter;
            strHTML += "<b>Manufacturer:</b> ";
            strHTML += manufacturer->m_strName;
            strHTML += "<br>";

            std::deque<CMDF_Address*>::iterator iterAddr;
            for (iterAddr = manufacturer->m_list_Address.begin();
                 iterAddr != manufacturer->m_list_Address.end();
                 ++iterAddr) {

                CMDF_Address* address = *iterAddr;
                strHTML += "<h4>Address</h4>";
                strHTML += "<b>Street:</b> ";
                strHTML += address->m_strStreet;
                strHTML += "<br>";
                strHTML += "<b>Town:</b> ";
                strHTML += address->m_strTown;
                strHTML += "<br>";
                strHTML += "<b>City:</b> ";
                strHTML += address->m_strCity;
                strHTML += "<br>";
                strHTML += "<b>Post Code:</b> ";
                strHTML += address->m_strPostCode;
                strHTML += "<br>";
                strHTML += "<b>State:</b> ";
                strHTML += address->m_strState;
                strHTML += "<br>";
                strHTML += "<b>Region:</b> ";
                strHTML += address->m_strRegion;
                strHTML += "<br>";
                strHTML += "<b>Country:</b> ";
                strHTML += address->m_strCountry;
                strHTML += "<br><br>";
            }

            std::deque<CMDF_Item*>::iterator iterPhone;
            for (iterPhone = manufacturer->m_list_Phone.begin();
                 iterPhone != manufacturer->m_list_Phone.end();
                 ++iterPhone) {

                CMDF_Item* phone = *iterPhone;
                strHTML += "<b>Phone:</b> ";
                strHTML += phone->m_strItem;
                strHTML += " ";
                strHTML += phone->m_strDescription;
                strHTML += "<br>";
            }

            std::deque<CMDF_Item*>::iterator iterFax;
            for (iterFax = manufacturer->m_list_Fax.begin();
                 iterFax != manufacturer->m_list_Fax.end();
                 ++iterFax) {

                CMDF_Item* fax = *iterFax;
                strHTML += "<b>Fax:</b> ";
                strHTML += fax->m_strItem;
                strHTML += " ";
                strHTML += fax->m_strDescription;
                strHTML += "<br>";
            }

            std::deque<CMDF_Item*>::iterator iterEmail;
            for (iterEmail = manufacturer->m_list_Email.begin();
                 iterEmail != manufacturer->m_list_Email.end();
                 ++iterEmail) {

                CMDF_Item* email = *iterEmail;
                strHTML += "<b>Email:</b> <a href=\"";
                strHTML += email->m_strItem;
                strHTML += "\" >";
                strHTML += email->m_strItem;
                strHTML += "</a> ";
                strHTML += email->m_strDescription;
                strHTML += "<br>";
            }

            std::deque<CMDF_Item*>::iterator iterWeb;
            for (iterWeb = manufacturer->m_list_Web.begin();
                 iterWeb != manufacturer->m_list_Web.end();
                 ++iterWeb) {

                CMDF_Item* web = *iterWeb;
                strHTML += "<b>Web:</b> <a href=\"";
                strHTML += web->m_strItem;
                strHTML += "\">";
                strHTML += web->m_strItem;
                strHTML += "</a> ";
                strHTML += web->m_strDescription;
                strHTML += "<br>";
            }

        } // manufacturer

    } else {
        strHTML += "No MDF info available.";
        strHTML += "<br>";
    }

    strHTML += "</font>";
    strHTML += "</body></html>";

    return strHTML;
}

// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
// vscp_getEncryptionCodeFromToken
//

uint8_t
vscp_getEncryptionCodeFromToken(std::string& token)
{
    uint8_t code    = 0;
    std::string str = token;
    vscp_makeUpper(str);
    vscp_trim(str);

    if (0 == vscp_strcasecmp(str.c_str(), VSCP_ENCRYPTION_TOKEN_1)) {
        code = VSCP_ENCRYPTION_AES128;
    } else if (0 == vscp_strcasecmp(str.c_str(), VSCP_ENCRYPTION_TOKEN_2)) {
        code = VSCP_ENCRYPTION_AES192;
    } else if (0 == vscp_strcasecmp(str.c_str(), VSCP_ENCRYPTION_TOKEN_3)) {
        code = VSCP_ENCRYPTION_AES256;
    } else {
        code = 0;
    }

    return code;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_getEncryptionTokenFromCode
//

void
vscp_getEncryptionTokenFromCode(uint8_t code, std::string& token)
{
    switch (code) {

        case VSCP_ENCRYPTION_NONE:
            token = VSCP_ENCRYPTION_TOKEN_0;
            break;

        case VSCP_ENCRYPTION_AES128:
            token = VSCP_ENCRYPTION_TOKEN_1;
            break;

        case VSCP_ENCRYPTION_AES192:
            token = VSCP_ENCRYPTION_TOKEN_2;
            break;

        case VSCP_ENCRYPTION_AES256:
            token = VSCP_ENCRYPTION_TOKEN_3;
            break;

        default:
            /* No encryption */
            token = VSCP_ENCRYPTION_TOKEN_0;
            break;
    }
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getUDpFrameSizeFromEvent
//

size_t
vscp_getFrameSizeFromEvent(vscpEvent* pEvent)
{
    // Check pointer
    if (NULL == pEvent)
        return false;

    size_t size = 1 + // Packet type
                  VSCP_MULTICAST_PACKET0_HEADER_LENGTH + pEvent->sizeData +
                  2; // CRC
    return size;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getUDpFrameSizeFromEventEx
//

size_t
vscp_getFrameSizeFromEventEx(vscpEventEx* pEventEx)
{
    // Check pointer
    if (NULL == pEventEx)
        return false;

    size_t size = 1 + // Packet type
                  VSCP_MULTICAST_PACKET0_HEADER_LENGTH + pEventEx->sizeData +
                  2; // CRC
    return size;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_writeEventToFrame
//

bool
vscp_writeEventToFrame(uint8_t* frame,
                       size_t len,
                       uint8_t pkttype,
                       const vscpEvent* pEvent)
{
    // Check pointers
    if (NULL == frame)
        return false;
    if (NULL == pEvent)
        return false;
    // Can't have data size with invalid data pointer
    if (pEvent->sizeData && (NULL == pEvent->pdata))
        return false;

    size_t calcSize = 1 + // Packet type
                      VSCP_MULTICAST_PACKET0_HEADER_LENGTH + pEvent->sizeData +
                      2; // CRC

    if (len < calcSize)
        return false;

    // Frame type
    frame[VSCP_MULTICAST_PACKET0_POS_PKTTYPE] = pkttype;

    // Packet type
    frame[VSCP_MULTICAST_PACKET0_POS_HEAD_MSB] = (pEvent->head >> 8) & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_HEAD_LSB] = pEvent->head & 0xff;

    // Timestamp
    frame[VSCP_MULTICAST_PACKET0_POS_TIMESTAMP] =
      (pEvent->timestamp >> 24) & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_TIMESTAMP + 1] =
      (pEvent->timestamp >> 16) & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_TIMESTAMP + 2] =
      (pEvent->timestamp >> 8) & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_TIMESTAMP + 3] = pEvent->timestamp & 0xff;

    // Date / time block GMT
    frame[VSCP_MULTICAST_PACKET0_POS_YEAR_MSB] = (pEvent->year >> 8) & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_YEAR_LSB] = pEvent->year & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_MONTH]    = pEvent->month;
    frame[VSCP_MULTICAST_PACKET0_POS_DAY]      = pEvent->day;
    frame[VSCP_MULTICAST_PACKET0_POS_HOUR]     = pEvent->hour;
    frame[VSCP_MULTICAST_PACKET0_POS_MINUTE]   = pEvent->minute;
    frame[VSCP_MULTICAST_PACKET0_POS_SECOND]   = pEvent->second;

    // Class
    frame[VSCP_MULTICAST_PACKET0_POS_VSCP_CLASS_MSB] =
      (pEvent->vscp_class >> 8) & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_VSCP_CLASS_LSB] =
      pEvent->vscp_class & 0xff;

    // Type
    frame[VSCP_MULTICAST_PACKET0_POS_VSCP_TYPE_MSB] =
      (pEvent->vscp_type >> 8) & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_VSCP_TYPE_LSB] = pEvent->vscp_type & 0xff;

    // GUID
    memcpy(frame + VSCP_MULTICAST_PACKET0_POS_VSCP_GUID, pEvent->GUID, 16);

    // Size
    frame[VSCP_MULTICAST_PACKET0_POS_VSCP_SIZE_MSB] =
      (pEvent->sizeData >> 8) & 0xff;
    frame[VSCP_MULTICAST_PACKET0_POS_VSCP_SIZE_LSB] = pEvent->sizeData & 0xff;

    // Data
    if (pEvent->sizeData) {
        memcpy(frame + VSCP_MULTICAST_PACKET0_POS_VSCP_DATA,
               pEvent->pdata,
               pEvent->sizeData);
    }

    // Calculate CRC
    crc framecrc =
      crcFast((unsigned char const*)frame + 1,
              VSCP_MULTICAST_PACKET0_HEADER_LENGTH + pEvent->sizeData);

    // CRC
    frame[1 + VSCP_MULTICAST_PACKET0_HEADER_LENGTH + pEvent->sizeData] =
      (framecrc >> 8) & 0xff;
    frame[1 + VSCP_MULTICAST_PACKET0_HEADER_LENGTH + pEvent->sizeData + 1] =
      framecrc & 0xff;

#if 0
    printf("CRC1 %02X %02X\n",
                frame[ 1 + VSCP_MULTICAST_PACKET0_HEADER_LENGTH + pEvent->sizeData ],
                frame[ 1 + VSCP_MULTICAST_PACKET0_HEADER_LENGTH + pEvent->sizeData + 1 ] );
    printf("CRC2 %02X %02X\n",
                ( framecrc >> 8 ) & 0xff,
                framecrc & 0xff );
    crc nnnn = crcFast( frame+1,
                            VSCP_MULTICAST_PACKET0_HEADER_LENGTH +
                            pEvent->sizeData );
    printf("CRC3 %02X %02X\n",
                ( nnnn >> 8 ) & 0xff,
                nnnn & 0xff );
    printf("--------------------------------\n");
#endif

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_writeEventExToFrame
//

bool
vscp_writeEventExToFrame(uint8_t* frame,
                         size_t len,
                         uint8_t pkttype,
                         const vscpEventEx* pEventEx)
{
    vscpEvent* pEvent;

    pEvent = new vscpEvent;
    if (NULL == pEvent)
        return false;
    pEvent->pdata = NULL;

    // Check pointer (rest is checked in vscp_convertEventExToEvent)
    if (NULL == pEventEx)
        return false;

    // Convert eventEx to event
    if (!vscp_convertEventExToEvent(pEvent, pEventEx))
        return false;

    if (!vscp_writeEventToFrame(frame, len, pkttype, pEvent))
        return false;
    vscp_deleteEvent_v2(&pEvent);

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_getEventFromFrame
//

bool
vscp_getEventFromFrame(vscpEvent* pEvent, const uint8_t* buf, size_t len)
{
    // Check pointers
    if (NULL == pEvent)
        return false;
    if (NULL == buf)
        return false;

    //  0           Packet type & encryption settings
    //  1           HEAD MSB
    //  2           HEAD LSB
    //  3           Timestamp microseconds MSB
    //  4           Timestamp microseconds
    //  5           Timestamp microseconds
    //  6           Timestamp microseconds LSB
    //  7           Year MSB
    //  8           Year LSB
    //  9           Month
    //  10           Day
    //  11          Hour
    //  12          Minute
    //  13          Second
    //  14          CLASS MSB
    //  15          CLASS LSB
    //  16          TYPE MSB
    //  17          TYPE LSB
    //  18 - 33     ORIGINATING GUID
    //  34          DATA SIZE MSB
    //  35          DATA SIZE LSB
    //  36 - n 	    data limited to max 512 - 25 = 487 bytes
    //  len - 2     CRC MSB( Calculated on HEAD + CLASS + TYPE + ADDRESS + SIZE
    //  + DATA ) len - 1     CRC LSB
    // if encrypted with AES128/192/256 16.bytes IV here.

    size_t calcFrameSize =
      1 +                                    // packet type & encryption
      VSCP_MULTICAST_PACKET0_HEADER_LENGTH + // header
      2 +                                    // CRC
      ((uint16_t)buf[VSCP_MULTICAST_PACKET0_POS_VSCP_SIZE_MSB] << 8) +
      buf[VSCP_MULTICAST_PACKET0_POS_VSCP_SIZE_LSB];

    // The buffer must hold a frame
    if (len < calcFrameSize)
        return false;

    crc crcFrame =
      ((uint16_t)buf[calcFrameSize - 2] << 8) + buf[calcFrameSize - 1];

    // CRC check (only if not disabled)
    crc crcnew;
    if (!((buf[VSCP_MULTICAST_PACKET0_POS_HEAD_LSB] & VSCP_HEADER_NO_CRC) &&
          (VSCP_NOCRC_CALC_DUMMY_CRC == crcFrame))) {

#if 0
    int i;
    printf("DUMP = ");
    for ( i=0; i<calcFrameSize; i++ ) {
        printf("%02X ", buf[i] );
    }
    printf("\n");
#endif

        // Calculate & check CRC
        crcnew = crcFast((unsigned char const*)buf + 1, calcFrameSize - 1);
        // CRC is zero if calculated over itself
        if (crcnew)
            return false;
    }

    pEvent->sizeData =
      ((uint16_t)buf[VSCP_MULTICAST_PACKET0_POS_VSCP_SIZE_MSB] << 8) +
      buf[VSCP_MULTICAST_PACKET0_POS_VSCP_SIZE_LSB];

    // Allocate data
    if (NULL == (pEvent->pdata = new uint8_t[pEvent->sizeData])) {
        return false;
    }

    // copy in data
    memcpy(pEvent->pdata,
           buf + VSCP_MULTICAST_PACKET0_POS_VSCP_DATA,
           pEvent->sizeData);

    // Head
    pEvent->head = ((uint16_t)buf[VSCP_MULTICAST_PACKET0_POS_HEAD_MSB] << 8) +
                   buf[VSCP_MULTICAST_PACKET0_POS_HEAD_LSB];

    // Copy in GUID
    memcpy(pEvent->GUID,
           buf + VSCP_MULTICAST_PACKET0_POS_VSCP_GUID,
           pEvent->sizeData);

    // Set CRC
    pEvent->crc = crcFrame;

    // Set timestamp
    pEvent->timestamp =
      ((uint32_t)buf[VSCP_MULTICAST_PACKET0_POS_TIMESTAMP] << 24) +
      ((uint32_t)buf[VSCP_MULTICAST_PACKET0_POS_TIMESTAMP + 1] << 16) +
      ((uint32_t)buf[VSCP_MULTICAST_PACKET0_POS_TIMESTAMP + 2] << 8) +
      buf[VSCP_MULTICAST_PACKET0_POS_TIMESTAMP + 3];

    // If timestamp is zero, set it
    if (0 == pEvent->timestamp) {
        pEvent->timestamp = vscp_makeTimeStamp();
    }

    // Date/time
    pEvent->year = ((uint16_t)buf[VSCP_MULTICAST_PACKET0_POS_YEAR_MSB] << 8) +
                   buf[VSCP_MULTICAST_PACKET0_POS_YEAR_LSB];
    pEvent->month  = buf[VSCP_MULTICAST_PACKET0_POS_MONTH];
    pEvent->day    = buf[VSCP_MULTICAST_PACKET0_POS_DAY];
    pEvent->hour   = buf[VSCP_MULTICAST_PACKET0_POS_HOUR];
    pEvent->minute = buf[VSCP_MULTICAST_PACKET0_POS_MINUTE];
    pEvent->second = buf[VSCP_MULTICAST_PACKET0_POS_SECOND];

    // If date/time field is zero set GMT now
    if ((0 == pEvent->year) && (0 == pEvent->month) && (0 == pEvent->day) &&
        (0 == pEvent->hour) && (0 == pEvent->minute) && (0 == pEvent->second)) {

        time_t rawtime;
        struct tm* ptm;

        time(&rawtime);
        ptm = gmtime(&rawtime);

        pEvent->year   = ptm->tm_year + 1900;
        pEvent->month  = ptm->tm_mon + 1;
        pEvent->day    = ptm->tm_mday;
        pEvent->hour   = ptm->tm_hour;
        pEvent->minute = ptm->tm_min;
        pEvent->second = ptm->tm_sec;
    }

    // VSCP Class
    pEvent->vscp_class =
      ((uint16_t)buf[VSCP_MULTICAST_PACKET0_POS_VSCP_CLASS_MSB] << 8) +
      buf[VSCP_MULTICAST_PACKET0_POS_VSCP_CLASS_LSB];

    // VSCP Type
    pEvent->vscp_type =
      ((uint16_t)buf[VSCP_MULTICAST_PACKET0_POS_VSCP_TYPE_MSB] << 8) +
      buf[VSCP_MULTICAST_PACKET0_POS_VSCP_TYPE_LSB];

    // obid - set to zero so interface fill it in
    pEvent->obid = 0;

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_getEventExFromFrame
//

bool
vscp_getEventExFromFrame(vscpEventEx* pEventEx,
                         const uint8_t* frame,
                         size_t len)
{
    vscpEvent* pEvent;

    pEvent = new vscpEvent;
    if (NULL == pEvent)
        return false;
    pEvent->pdata = NULL;

    // Check pointer (rest is checked in vscp_getVscpEventFromUdpFrame)
    if (NULL == pEventEx)
        return false;

    if (!vscp_getEventFromFrame(pEvent, frame, len)) {
        return false;
    }

    // Convert eventEx to event
    if (vscp_convertEventToEventEx(pEventEx, pEvent))
        return false;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_encryptFrame
//

size_t
vscp_encryptFrame(uint8_t* output,
                  uint8_t* input,
                  size_t len,
                  const uint8_t* key,
                  const uint8_t* iv,
                  uint8_t nAlgorithm)
{
    uint8_t generated_iv[16];

    if (NULL == output)
        return 0;
    if (NULL == input)
        return 0;
    if (NULL == key)
        return 0;

    // If no encryption needed - return
    if (VSCP_ENCRYPTION_NONE == nAlgorithm) {
        memcpy(output, input, len);
        return len;
    }

    // Must pad if needed
    size_t padlen = len - 1; // Without packet type
    padlen        = len + (16 - (len % 16));

    // The packet type s always un encrypted
    output[0] = input[0];

    // Should decryption algorithm be set by package
    if (VSCP_ENCRYPTION_FROM_TYPE_BYTE == (nAlgorithm & 0x0f )) {
        nAlgorithm = input[0] & 0x0f;
    }

    // If iv is not give it should be generated
    if (NULL == iv) {
        if (16 != getRandomIV(generated_iv, 16))
            return 0;
    } else {
        memcpy(generated_iv, iv, 16);
    }

    switch (nAlgorithm) {

        case VSCP_ENCRYPTION_AES192:
            AES_CBC_encrypt_buffer(AES192,
                                   output + 1,
                                   input + 1, // Not Packet type byte
                                   padlen,
                                   key,
                                   (const uint8_t*)generated_iv);
            // Append iv
            memcpy(output + 1 + padlen, generated_iv, 16);
            padlen += 16;
            break;

        case VSCP_ENCRYPTION_AES256:
            AES_CBC_encrypt_buffer(AES256,
                                   output + 1,
                                   input + 1, // Not Packet type byte
                                   padlen,
                                   key,
                                   (const uint8_t*)generated_iv);
            // Append iv
            memcpy(output + 1 + padlen, generated_iv, 16);
            padlen += 16;
            break;

        case VSCP_ENCRYPTION_AES128:
            AES_CBC_encrypt_buffer(AES128,
                                   output + 1,
                                   input + 1, // Not Packet type byte
                                   padlen,
                                   key,
                                   (const uint8_t*)generated_iv);
            // Append iv
            memcpy(output + 1 + padlen, generated_iv, 16);
            padlen += 16;
            break;

        default:
        case VSCP_ENCRYPTION_NONE:
            memcpy(output + 1, input + 1, padlen);
            break;
    }

    padlen++; // Count packet type byte

    return padlen;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_encryptFrame
//

bool
vscp_decryptFrame(uint8_t* output,
                  uint8_t* input,
                  size_t len,
                  const uint8_t* key,
                  const uint8_t* iv,
                  uint8_t nAlgorithm)
{
    uint8_t appended_iv[16];
    size_t real_len = len;

    if (NULL == output)
        return false;
    if (NULL == input)
        return false;
    if (NULL == key)
        return false;

    if (VSCP_ENCRYPTION_NONE ==
        GET_VSCP_MULTICAST_PACKET_ENCRYPTION(nAlgorithm)) {
        memcpy(output, input, len);
        return true;
    }

    // If iv is not given it should be fetched from the end of input (last 16
    // bytes)
    if (NULL == iv) {
        memcpy(appended_iv, (input + len - 16), 16);
        real_len -= 16; // Adjust frame length accordingly
    } else {
        memcpy(appended_iv, iv, 16);
    }

    // Preserve packet type which always is un-encrypted
    output[0] = input[0];

    // Should decryption algorithm be set by package
    if (VSCP_ENCRYPTION_FROM_TYPE_BYTE == (nAlgorithm & 0x0f )) {
        nAlgorithm = input[0] & 0x0f;
    }

    switch (nAlgorithm) {

        case VSCP_ENCRYPTION_AES256:
            AES_CBC_decrypt_buffer(AES256,
                                   output + 1,
                                   input + 1,
                                   real_len - 1,
                                   key,
                                   (const uint8_t*)appended_iv);
            break;

        case VSCP_ENCRYPTION_AES192:
            AES_CBC_decrypt_buffer(AES192,
                                   output + 1,
                                   input + 1,
                                   real_len - 1,
                                   key,
                                   (const uint8_t*)appended_iv);
            break;

        default:
        case VSCP_ENCRYPTION_AES128:
            AES_CBC_decrypt_buffer(AES128,
                                   output + 1,
                                   input + 1,
                                   real_len - 1,
                                   key,
                                   (const uint8_t*)appended_iv);
            break;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////
//                           Password/key handling
///////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// vscp_md5
//

void
vscp_md5(char* digest, const unsigned char* buf, size_t len)
{
    unsigned char hash[16];

    md5_state_s pms;

    vscpmd5_init(&pms);
    vscpmd5_append(&pms, buf, len);
    vscpmd5_finish(&pms, hash);
    vscp_byteArray2HexStr(digest, hash, sizeof(hash));
}

///////////////////////////////////////////////////////////////////////////////
// vscp_bin2str
//
// Stringify binary data. Output buffer must be twice as big as input,
// because each byte takes 2 bytes in string representation
//

void
vscp_byteArray2HexStr(char* to, const unsigned char* p, size_t len)
{
    static const char* hex = "0123456789abcdef";

    for (; len--; p++) {
        *to++ = hex[p[0] >> 4];
        *to++ = hex[p[0] & 0x0f];
    }

    *to = '\0';
}

////////////////////////////////////////////////////////////////////////////////
// vscp_hexStr2ByteArray
//

size_t
vscp_hexStr2ByteArray(uint8_t* array, size_t size, const char* hexstr)
{
    int slen = strlen(hexstr);
    int i = 0, j = 0;

    // The output array size is half the hex_str length (rounded up)
    size_t nhexsize = (slen + 1) / 2;

    if (size < nhexsize) {
        // Too big for the output array
        return 0;
    }

    if (slen % 2 == 1) {
        // hex_str is an odd length, so assume an implicit "0" prefix
        if (sscanf(&(hexstr[0]), "%1hhx", &(array[0])) != 1) {
            return 0;
        }

        i = j = 1;
    }

    for (; i < slen; i += 2, j++) {
        if (sscanf(&(hexstr[i]), "%2hhx", &(array[j])) != 1) {
            return 0;
        }
    }

    return nhexsize;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_getHashPasswordComponents
//

bool
vscp_getHashPasswordComponents(uint8_t* pSalt,
                               uint8_t* pHash,
                               const std::string& stored_pw)
{
    std::string strSalt;
    std::string strHash;

    // Check pointers
    if (NULL == pSalt)
        return false;
    if (NULL == pHash)
        return false;

    std::deque<std::string> tokens;
    vscp_split(tokens, stored_pw, ";");
    if (2 != tokens.size())
        return false;

    strSalt = tokens.front();
    tokens.pop_front();
    vscp_hexStr2ByteArray(pSalt, 16, strSalt.c_str());

    strHash = tokens.front();
    tokens.pop_front();
    vscp_hexStr2ByteArray(pHash, 32, strHash.c_str());

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_makePasswordHash
//

bool
vscp_makePasswordHash(std::string& result,
                      const std::string& password,
                      uint8_t* pSalt)
{
    int i;
    uint8_t salt[16];
    uint8_t buf[32];

    result.clear();

    // Get random IV
    if (NULL == pSalt) {
        if (16 != getRandomIV(salt, 16)) {
            return false;
        }
    } else {
        memcpy(salt, pSalt, 16);
    }

    uint8_t* p = new uint8_t[strlen((const char*)password.c_str())];
    if (NULL == p)
        return false;

    memcpy(p,
           (const char*)password.c_str(),
           strlen((const char*)password.c_str()));

    fastpbkdf2_hmac_sha256(p,
                           strlen((const char*)password.c_str()),
                           salt,
                           16,
                           70000,
                           buf,
                           32);
    delete[] p;

    for (i = 0; i < 16; i++) {
        result += vscp_str_format("%02X", salt[i]);
    }
    result += (";");
    for (i = 0; i < 32; i++) {
        result += vscp_str_format("%02X", buf[i]);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////
// vscp_isPasswordValid
//

bool
vscp_isPasswordValid(const std::string& stored_pw, const std::string& password)
{
    std::string calcHash; // Calculated hash
    uint8_t salt[16];     // Stored salt
    uint8_t hash[32];     // Stored hash

    if (!vscp_getHashPasswordComponents(salt, hash, stored_pw)) {
        return false;
    }

    if (!vscp_makePasswordHash(calcHash, password, salt)) {
        return false;
    }

    if (stored_pw != calcHash)
        return false;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getSalt
//

bool
vscp_getSalt(uint8_t* buf, size_t len)
{
    if (!getRandomIV(buf, len))
        return false;
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// vscp_getSaltHex
//

bool
vscp_getSaltHex(std::string& strSalt, size_t len)
{
    if (len) {
        uint8_t* pbuf = new uint8_t[len];
        if (len != getRandomIV(pbuf, len)) {
            delete[] pbuf;
            return false;
        }

        strSalt.clear();
        for (size_t i = 0; i < len; i++) {
            strSalt += vscp_str_format("%02X", pbuf[i]);
        }

        delete[] pbuf;
    }

    return true;
}
