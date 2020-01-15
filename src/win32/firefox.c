#include <stdlib.h>
#include <stdio.h>

#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <prtypes.h> 

#include "main.h"
#include "firefox.h"
#include "firefox_win.h"
#include "functions.h"

// We define some variables to hold our functions
NSSInit NSS_Init;
PK11GetInternalKeySlot PK11_GetInternalKeySlot;
PK11SDRDecrypt PK11SDR_Decrypt;
NSSBase64DecodeBuffer NSSBase64_DecodeBuffer;
PK11Authenticate PK11_Authenticate;
PK11CheckUserPassword PK11_CheckUserPassword;
NSSShutdown NSS_Shutdown;
PK11FreeSlot PK11_FreeSlot;
HMODULE moduleNSS;

/** 
 * Load Windows Required libraries
 *
 * @return 1 on success, -1 on failure
 */
int load_firefox_libs() {
        char pathFirefox[MAX_PATH_SIZE];
        char pathDll[MAX_PATH_SIZE];
        char new_path[MAX_PATH_SIZE];
        SHGetSpecialFolderPath(0, pathFirefox, CSIDL_PROGRAM_FILES, FALSE);
        strcat(pathFirefox, "\\Mozilla Firefox");

	// We set our ENV PATH to load all libary dependencies.
	char *path = getenv("PATH");
	if (path){
		snprintf(new_path, MAX_PATH_SIZE, "PATH=%s;%s", path, pathFirefox);
		_putenv(new_path);
	}
	path=getenv("PATH");

        snprintf(pathDll, MAX_PATH_SIZE, "%s\\%s", pathFirefox, "nss3.dll");
        if(!(moduleNSS = LoadLibrary(pathDll))) {
		fprintf(stderr, "nss3.dll Loading failure\n");
                return -1;
	}
 
        NSS_Init = (NSSInit)GetProcAddress(moduleNSS, "NSS_Init");
        PK11_GetInternalKeySlot = (PK11GetInternalKeySlot)GetProcAddress(moduleNSS, "PK11_GetInternalKeySlot");
        PK11_Authenticate = (PK11Authenticate)GetProcAddress(moduleNSS, "PK11_Authenticate");
        PK11SDR_Decrypt = (PK11SDRDecrypt)GetProcAddress(moduleNSS, "PK11SDR_Decrypt");
        //NSSBase64_DecodeBuffer = (NSSBase64DecodeBuffer)GetProcAddress(moduleNSS, "NSSBase64_DecodeBuffer");
	PL_Base64Decode = (fpPL_Base64Decode)GetProcAddress(moduleNSS, "PL_Base64Decode");
        PK11_CheckUserPassword = (PK11CheckUserPassword)GetProcAddress(moduleNSS, "PK11_CheckUserPassword");
        NSS_Shutdown = (NSSShutdown)GetProcAddress(moduleNSS, "NSS_Shutdown");
        PK11_FreeSlot = (PK11FreeSlot)GetProcAddress(moduleNSS, "PK11_FreeSlot");

	// Added
	//SECItem_FreeItem = (SECItemFreeItem)GetProcAddress(moduleNSS, "SECItem_FreeItem");

	if(!NSS_Init || !PK11_GetInternalKeySlot || !PK11_Authenticate || !PK11SDR_Decrypt || !PL_Base64Decode || !PK11_CheckUserPassword || !NSS_Shutdown || !PK11_FreeSlot) {
		fprintf(stderr, "GetProcAddress() failure\n");
		FreeLibrary(moduleNSS);
		return -1;
	}

        return 1;
}

/**
 * Get the Firefox profile path
 *
 * @return 1 on success, -1 on failure
 */
int get_profile(char* profiles_ini_path, char* profile) {
	GetPrivateProfileString("Profile0", "Path", "", profile, MAX_PATH_SIZE, profiles_ini_path);
	return 1;
}

/** 
 * Load Windows Firefox paths
 *
 * @return 1 on success, -1 on failure
 */
int load_firefox_paths(char *firefox_path, char *profiles_ini_path) {
	SHGetSpecialFolderPath(0, firefox_path, CSIDL_APPDATA, FALSE); 
	strcat(firefox_path, "\\Mozilla\\Firefox");
	snprintf(profiles_ini_path, MAX_PATH_SIZE, "%s\\profiles.ini", firefox_path);

	return 1;
}

/**
 * Decrypt firefox ciphered password
 *
 * @return 1 on success, -1 on failure
 */
int decrypt_firefox_cipher(char *ciphered, char **plaintext) {
	// TODO: See if we can use NSSBase64_DecodeBuffer()
	// TODO: See if we can use SECItem_FreeItem() to free items after we finished
	// TODO: See if we can use SECITEM_AllocItem()
	SECItem request;
	SECItem response;
	unsigned int len = calc_base64_length(ciphered);

	request.data = malloc(len+1);
	request.len = len;
	memset(request.data, 0x0, len + 1);

	char *decoded_cipher = (char *)malloc(len+1);
	if(decoded_cipher == 0) {
		fprintf(stderr, "malloc() failure\n");
		free(request.data);
		free(decoded_cipher);
		return -1;
	}
	memset(decoded_cipher, 0, len+1);

	//response = SECITEM_AllocItem(NULL, NULL, 0);
	//request = NSSBase64_DecodeBuffer(NULL, NULL, ciphered, len);

	if(PL_Base64Decode(ciphered, strlen(ciphered), decoded_cipher) == 0) {
		fprintf(stderr, "PL_Base64Decode() failure\n");
		free(request.data);
		return -1;
	}
	memcpy(request.data, decoded_cipher, len);
	free(decoded_cipher);

	if(PK11SDR_Decrypt(&request, &response, NULL) == -1) {
		fprintf(stderr, "PK11SDR_Decrypt() failure\n");
		return -1;
	}

	*plaintext = malloc(response.len + 1);
	if(*plaintext == 0) {
		fprintf(stderr, "malloc() failure\n");
		free(*plaintext);
		return -1;
	}
	strncpy(*plaintext, (const char * restrict)response.data, response.len);
	(*plaintext)[response.len] = '\0';

	//SECItem_FreeItem(&request, TRUE);
	//SECItem_FreeItem(&response, TRUE); 

	return 1;
}

/**
 * Authenticate via NSS to be able to query passwords
 *
 * @return 1 on success, -1 on failure
 */
int nss_authenticate(char *profile_path, void *key_slot, const char *master_password) {
	load_firefox_libs();
	if(NSS_Init(profile_path) != SECSuccess) {
		fprintf(stderr, "NSS Initialisation failed\n");
		fflush(stderr);
		return -1;
	}

	// We get the key[3-4].db file
	if((key_slot = PK11_GetInternalKeySlot()) == NULL) {
		fprintf(stderr, "PK11_GetInternalKeySlot() failed\n");
		fflush(stderr);
		return -1;
	}

	if(master_password != NULL) {
		if(PK11_CheckUserPassword(key_slot, master_password) != SECSuccess) {
			fprintf(stderr, "PK11_CheckUserPassword() failed, Wrong master password\n");
			fflush(stderr);
			return -1;
		}
	} else {
		// We check if we can open it with no password
		if(PK11_CheckUserPassword(key_slot, "") != SECSuccess) {
			fprintf(stderr, "PK11_CheckUserPassword() failed, Try with -m <PASSWORD> option\n");
			fflush(stderr);
			return -1;
		}
	}

	if(PK11_Authenticate(key_slot, TRUE, NULL) != SECSuccess) {
		fprintf(stderr, "PK11_Authenticate() failed\n");
		fflush(stderr);
		return -1;
	}

	return 1;
}

/**
 * Free PK11 / NSS Functions
 *
 * @return 
 */
void free_pk11_nss(void *key_slot) {
	PK11_FreeSlot(&key_slot);
	NSS_Shutdown();
	FreeLibrary(moduleNSS);
}
