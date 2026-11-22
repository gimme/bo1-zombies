#pragma once
#include "windows.h"
typedef ULONG_PTR HCRYPTPROV;
#define PROV_RSA_FULL 1
#define MS_DEF_PROV "Microsoft Base Cryptographic Provider v1.0"
#define CRYPT_VERIFYCONTEXT 0xF0000000
#define CRYPT_NEWKEYSET 0x00000008
#define CRYPT_MACHINE_KEYSET 0x00000020
BO1_EXTERN_C BOOL WINAPI CryptAcquireContextA(HCRYPTPROV *phProv, LPCSTR szContainer, LPCSTR szProvider, DWORD dwProvType, DWORD dwFlags);
BO1_EXTERN_C BOOL WINAPI CryptGenRandom(HCRYPTPROV hProv, DWORD dwLen, BYTE *pbBuffer);
BO1_EXTERN_C BOOL WINAPI CryptReleaseContext(HCRYPTPROV hProv, DWORD dwFlags);
#define CryptAcquireContext CryptAcquireContextA
