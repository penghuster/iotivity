//******************************************************************
//
// Copyright 2017 Samsung Electronics All Rights Reserved.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "iotivity_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pkcs12.h>
#include <mbedtls/ssl_internal.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "utlist.h"
#include "cJSON.h"
#include "base64.h"
#include "cainterface.h"
#include "ocstack.h"
#include "oic_malloc.h"
#include "oic_string.h"
#include "ocpayload.h"
#include "ocpayloadcbor.h"
#include "payload_logging.h"
#include "secureresourcemanager.h"
#include "secureresourceprovider.h"
#include "srmresourcestrings.h"
#include "srmutility.h"
#include "aclresource.h"
#include "pstatresource.h"
#include "doxmresource.h"
#include "amaclresource.h"
#include "credresource.h"
#include "security_internals.h"
#include "psinterface.h"
#include "pinoxmcommon.h"

#define TAG  "OIC_SVR_DB_EDITOR"

#define BOLD_BEGIN    "\033[1m"
#define RED_BEGIN      "\033[1;31m"
#define YELLOW_BEGIN  "\033[1;33m"
#define CYAN_BEGIN  "\033[1;36m"
#define GREEN_BEGIN  "\033[1;92m"
#define COLOR_END      "\033[0m"
#define COLOR_END_NL      "\033[0m\n"

#define ACL_PEMISN_CNT (5)
#define SVR_MAX_ENTITY (16)

#define SVR_DB_PATH_LENGTH 1024
#define PRINT_ERR(fmt,...) printf(RED_BEGIN "error: " fmt COLOR_END_NL, ##__VA_ARGS__)
#define PRINT_WARN(fmt,...) printf(YELLOW_BEGIN "warning : " fmt COLOR_END_NL, ##__VA_ARGS__)
#define PRINT_INFO(fmt,...) printf(YELLOW_BEGIN fmt COLOR_END_NL, ##__VA_ARGS__)
#define PRINT_PROG(fmt,...) printf(BOLD_BEGIN fmt COLOR_END, ##__VA_ARGS__)
#define PRINT_DATA(fmt,...) printf(CYAN_BEGIN fmt COLOR_END, ##__VA_ARGS__)
#define PRINT_NORMAL(fmt,...) printf(fmt, ##__VA_ARGS__)
#define PRINT_NL() printf("\n");

typedef enum OperationType
{
    SVR_PRINT_ALL = 1,
    SVR_EDIT_CRED = 2,
    SVR_EDIT_ACL = 3,
    SVR_EDIT_DOXM = 4,
    SVR_EDIT_PSTAT = 5,
    EXIT = 99
} OperationType_t;

typedef enum SubOperationType
{
    SVR_PRINT = 1,
    SVR_ADD = 2,
    SVR_REMOVE = 3,
    SVR_MODIFY = 4,
    SVR_EDIT_IDX_SIZE = 5,
    BACK = 99
} SubOperationType_t;

bool g_allowedEditMenu[SVR_EDIT_IDX_SIZE] = {false/*unused*/, false, false, false, false};
char g_svrDbPath[SVR_DB_PATH_LENGTH] = {0};

OicSecDoxm_t *g_doxmResource = NULL;
OicSecPstat_t *g_pstatResource = NULL;
OicSecAcl_t *g_aclResource = NULL;


/**
 * input |g_svr_db_fname| internally by force, not using |path| parameter
 * because |OCPersistentStorage::open| is called |OCPersistentStorage| internally
 * with its own |SVR_DB_FILE_NAME|
 */
static FILE *fopen_svreditor(const char *path, const char *mode)
{
    (void)path;  // unused |path| parameter

    return fopen(g_svrDbPath, mode);
}

static void RefreshSVRInstance(bool credRefFlag, bool aclRefFlag, bool doxmRefFlag,
                               bool pstatRefFlag);
static int ReadDataFromFile(const char *infoTxt, uint8_t **buffer, size_t *bufferSize);

static void FreeACE(OicSecAce_t *ace);

static void PrintMainMenu();
static void PrintEditMenu(const char *resourceName, bool print, bool add, bool remove, bool modify);
static void PrintUuid(const OicUuid_t *uuid);
static void PrintIntArray(const int *array, size_t length);
static void PrintStringArray(const char **array, size_t length);
static void PrintInt(int value);
static void PrintString(const char *text);
static void PrintBuffer(const uint8_t *buf, size_t bufLen);
static void PrintDpm(const OicSecDpm_t dpm);
static void PrintDpom(const OicSecDpom_t dpom);
#ifdef MULTIPLE_OWNER
static void PrintMom(const OicSecMom_t *mom);
#endif
static void PrintCredType(OicSecCredType_t credType);
static void PrintCredEncodingType(OicEncodingType_t encoding);
static void PrintHelp();

static void PrintResourceList(const OicSecRsrc_t *rsrcList);
static void PrintValidity(const OicSecValidity_t *validity);
static void PrintPermission(uint16_t permission);


static void PrintDoxm(const OicSecDoxm_t *doxm);
static void PrintPstat(const OicSecPstat_t *pstat);
static size_t PrintAcl(const OicSecAcl_t *acl);
/**
 * Print credential list.
 */
static void PrintCredList(const OicSecCred_t *creds);


static int InputNumber(const char *infoText);
static char InputChar(const char *infoText);
static char *InputString(const char *infoText);
static int InputUuid(OicUuid_t *uuid);


static int InputResources(OicSecRsrc_t *resources);
static int InputAceData(OicSecAce_t *ace);

static int InputCredUsage(char **credUsage);
static int InputCredEncodingType(const char *dataType, OicEncodingType_t *encoding);
static int InputCredentialData(OicSecCred_t *cred);

static void HandleCredOperation(SubOperationType_t cmd);
static void HandleAclOperation(const SubOperationType_t cmd);
static void HandleDoxmOperation(const SubOperationType_t cmd);
static void HandlePstatOperation(const SubOperationType_t cmd);


/**
 * Parse chain of X.509 certificates.
 *
 * @param[out] crt     container for X.509 certificates
 * @param[in]  buf     buffer with X.509 certificates. Certificates may be in either in PEM
 or DER format in a jumble. Each PEM certificate must be NULL-terminated.
 * @param[in]  bufLen  buffer length
 *
 * @return  0 on success, -1 on error
 */
static int ParseCertChain(mbedtls_x509_crt *crt, unsigned char *buf, size_t bufLen);
static void ParseDerCaCert(ByteArray_t *crt, const char *usage, uint16_t credId);
static void ParseDerKey(ByteArray_t *key, const char *usage, uint16_t credId);
static void ParseDerOwnCert(ByteArray_t *crt, const char *usage, uint16_t credId);


static int MainOperation(const char *svrpath)
{
    OperationType_t menu = EXIT;
    SubOperationType_t editMenu = EXIT;
    OCStackResult ocResult = OC_STACK_ERROR;
    bool run = true;

    // initialize persistent storage for SVR DB
    static OCPersistentStorage psInst =
    {
        .open = fopen_svreditor,
        .read = fread,
        .write = fwrite,
        .close = fclose,
        .unlink = unlink
    };

    if (!svrpath)
    {
        PRINT_ERR("Incorrect file path");
        return -1;
    }

    strcpy(g_svrDbPath, svrpath);

    ocResult = OCRegisterPersistentStorageHandler(&psInst);
    if (OC_STACK_OK != ocResult)
    {
        PRINT_ERR("OCRegisterPersistentStorageHandler : %d", ocResult);
        return -1;
    }

    RefreshSVRInstance(true, true, true, true);

    while (run)
    {
        PrintMainMenu();
        menu = (OperationType_t)InputNumber("\tSelect the menu : ");
        switch (menu)
        {
            case SVR_PRINT_ALL:
                PrintDoxm(g_doxmResource);
                PrintPstat(g_pstatResource);
                PrintAcl(g_aclResource);
                PrintCredList(GetCredList());
                break;
            case SVR_EDIT_CRED:
                do
                {
                    if (NULL == GetCredList())
                    {
                        PRINT_WARN("Credential resource is empty.");
                        PrintEditMenu("Credential Resource", false, true, false, false);
                    }
                    else
                    {
                        PrintEditMenu("Credential Resource", true, true, true, true);
                    }
                    editMenu = (SubOperationType_t)InputNumber("Select the menu : ");
                    HandleCredOperation(editMenu);
                    RefreshSVRInstance(true, false, false, false);
                }
                while (BACK != editMenu);
                break;
            case SVR_EDIT_ACL:
                do
                {
                    if (NULL == g_aclResource)
                    {
                        PRINT_WARN("ACL resource is empty.");
                        PrintEditMenu("ACL Resource", false, true, false, false);
                    }
                    else
                    {
                        PrintEditMenu("ACL Resource", true, true, true, false);
                    }
                    editMenu = (SubOperationType_t)InputNumber("Select the menu : ");
                    HandleAclOperation(editMenu);
                    RefreshSVRInstance(false, true, false, false);
                }
                while (BACK != editMenu);

                break;
            case SVR_EDIT_DOXM:
                PRINT_INFO("NOT SUPPORTED YET");
                //PrintEditMenu("Doxm Resource", false, false, true);
                //T.B.D
                break;
            case SVR_EDIT_PSTAT:
                PRINT_INFO("NOT SUPPORTED YET");
                //PrintEditMenu("Pstat Resource", false, false, true);
                //T.B.D
                break;
            case EXIT:
                run = false;
                break;
            default:
                PRINT_ERR("Unknown operation");
                PRINT_ERR("Please make sure the menu.");
                break;
        }
    }

    DeInitCredResource();
    DeletePstatBinData(g_pstatResource);
    DeleteDoxmBinData(g_doxmResource);
    DeleteACLList(g_aclResource);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc == 2)
    {
        PRINT_NORMAL("SVR DB File Path: %s\n", argv[1]);
        return MainOperation(argv[1]);
    }
    else
    {
        PrintHelp();
        return 0;
    }
}



///////////////////////////// Internal API implementation /////////////////////////////


static void RefreshSVRInstance(bool credRefFlag, bool aclRefFlag, bool doxmRefFlag,
                               bool pstatRefFlag)
{
    uint8_t *secPayload = NULL;
    size_t payloadSize = 0;
    OCStackResult ocResult = OC_STACK_ERROR;

    if (credRefFlag)
    {
        OicSecCred_t *credList = NULL;
        OicSecCred_t *cred = NULL;
        OicSecCred_t *tmpCred = NULL;
        //Load security resouce data from SVR DB.
        ocResult = GetSecureVirtualDatabaseFromPS(OIC_JSON_CRED_NAME, &secPayload, &payloadSize);
        if (OC_STACK_OK != ocResult)
        {
            PRINT_WARN("GetSecureVirtualDatabaseFromPS return %d", ocResult);
        }
        if (secPayload && 0 != payloadSize)
        {
            ocResult = CBORPayloadToCred(secPayload, payloadSize, &credList);
            if (OC_STACK_OK != ocResult)
            {
                OICFree(secPayload);
                PRINT_ERR("CBORPayloadToCred : %d", ocResult);
                return;
            }
        }
        OICFree(secPayload);
        secPayload = NULL;

        //Add the loaded credentials into gCred of CredResource module in order to use the credential management mechanism.
        LL_FOREACH_SAFE(credList, cred, tmpCred)
        {
            ocResult = AddCredential(cred);
            if (OC_STACK_OK != ocResult)
            {
                PRINT_ERR("AddCredential : %d", ocResult);
                return;
            }
        }
    }

    if (aclRefFlag)
    {
        ocResult = GetSecureVirtualDatabaseFromPS(OIC_JSON_ACL_NAME, &secPayload, &payloadSize);
        if (OC_STACK_OK != ocResult)
        {
            PRINT_WARN("GetSecureVirtualDatabaseFromPS return %d", ocResult);
        }

        if (g_aclResource)
        {
            DeleteACLList(g_aclResource);
            g_aclResource = NULL;
        }

        g_aclResource = CBORPayloadToAcl(secPayload, payloadSize);
        if (NULL == g_aclResource)
        {
            OICFree(secPayload);
            PRINT_ERR("Failed CBORPayloadToAcl");
            return;
        }

        OICFree(secPayload);
        secPayload = NULL;
    }

    if (doxmRefFlag)
    {
        ocResult = GetSecureVirtualDatabaseFromPS(OIC_JSON_DOXM_NAME, &secPayload, &payloadSize);
        if (OC_STACK_OK != ocResult)
        {
            PRINT_WARN("GetSecureVirtualDatabaseFromPS error : %d", ocResult);
        }

        if (g_doxmResource)
        {
            DeleteDoxmBinData(g_doxmResource);
            g_doxmResource = NULL;
        }

        ocResult = CBORPayloadToDoxm(secPayload, payloadSize, &g_doxmResource);
        if (OC_STACK_OK != ocResult)
        {
            OICFree(secPayload);
            PRINT_ERR("CBORPayloadToDoxm : %d", ocResult);
            return;
        }

        OICFree(secPayload);
        secPayload = NULL;
    }

    if (pstatRefFlag)
    {
        ocResult = GetSecureVirtualDatabaseFromPS(OIC_JSON_PSTAT_NAME, &secPayload, &payloadSize);
        if (OC_STACK_OK != ocResult)
        {
            PRINT_WARN("GetSecureVirtualDatabaseFromPS error : %d", ocResult);
        }

        if (g_pstatResource)
        {
            DeletePstatBinData(g_pstatResource);
            g_pstatResource = NULL;
        }

        ocResult = CBORPayloadToPstat(secPayload, payloadSize, &g_pstatResource);
        if (OC_STACK_OK != ocResult)
        {
            OICFree(secPayload);
            PRINT_ERR("CBORPayloadToPstat : %d", ocResult);
            return;
        }

        OICFree(secPayload);
        secPayload = NULL;
    }
}

static void FreeACE(OicSecAce_t *ace)
{
    if (NULL == ace)
    {
        OIC_LOG(ERROR, TAG, "Invalid Parameter");
        return;
    }

    //Clean Resources
    OicSecRsrc_t *rsrc = NULL;
    OicSecRsrc_t *tmpRsrc = NULL;
    LL_FOREACH_SAFE(ace->resources, rsrc, tmpRsrc)
    {
        LL_DELETE(ace->resources, rsrc);
        FreeRsrc(rsrc);
    }

    //Clean Validities
    OicSecValidity_t *validity = NULL;
    OicSecValidity_t *tmpValidity = NULL;
    LL_FOREACH_SAFE(ace->validities, validity, tmpValidity)
    {
        LL_DELETE(ace->validities, validity);

        //Clean period
        OICFree(validity->period);

        //Clean recurrence
        for (size_t i = 0; i < validity->recurrenceLen; i++)
        {
            OICFree(validity->recurrences[i]);
        }
        OICFree(validity->recurrences);
        OICFree(validity);
        validity = NULL;
    }

#ifdef MULTIPLE_OWNER
    OICFree(ace->eownerID);
#endif

    //Clean ACE
    OICFree(ace);
    ace = NULL;
}


static void PrintUuid(const OicUuid_t *uuid)
{
    char *strUuid = NULL;
    if (OC_STACK_OK == ConvertUuidToStr(uuid, &strUuid))
    {
        PRINT_DATA("%s\n", strUuid);
        OICFree(strUuid);
    }
    else
    {
        PRINT_ERR("Can not convert UUID to String");
    }
}

static void PrintIntArray(const int *array, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        PRINT_DATA("%d ", array[i]);
    }
    PRINT_NL();
}

static void PrintStringArray(const char **array, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        PRINT_DATA("%s ", array[i]);
    }
    PRINT_NL();
}

static void PrintInt(int value)
{
    PRINT_DATA("%d\n", value);
}

static void PrintString(const char *text)
{
    PRINT_DATA("%s\n", text);
}

static void PrintBuffer(const uint8_t *buf, size_t bufLen)
{
    size_t i = 0;

    for (i = 0; i < bufLen; i++)
    {
        if ((i + 1) % 20 == 0 || i == bufLen - 1)
        {
            PRINT_DATA("%02X \n", buf[i]);
        }
        else
        {
            PRINT_DATA("%02X ", buf[i]);
        }
    }
}

static void PrintDpm(const OicSecDpm_t dpm)
{
    PRINT_DATA("%d (", dpm);

    if (dpm == NORMAL)
    {
        PRINT_DATA(" NORMAL ");
    }
    if (dpm & RESET)
    {
        PRINT_DATA(" RESET ");
    }
    if (dpm & TAKE_OWNER)
    {
        PRINT_DATA(" TAKE_OWNER ");
    }
    if (dpm & BOOTSTRAP_SERVICE)
    {
        PRINT_DATA(" BOOTSTRAP_SERVICE ");
    }
    if (dpm & SECURITY_MANAGEMENT_SERVICES)
    {
        PRINT_DATA(" SECURITY_MANAGEMENT_SERVICES ");
    }
    if (dpm & PROVISION_CREDENTIALS)
    {
        PRINT_DATA(" PROVISION_CREDENTIALS ");
    }
    if (dpm & PROVISION_ACLS)
    {
        PRINT_DATA(" PROVISION_ACLS ");
    }
    PRINT_DATA(") \n");
}

static void PrintDpom(const OicSecDpom_t dpom)
{
    PRINT_DATA("%d (", dpom);

    if (dpom & MULTIPLE_SERVICE_SERVER_DRIVEN)
    {
        PRINT_DATA(" MULTIPLE_SERVICE_SERVER_DRIVEN ");
    }
    if (dpom & SINGLE_SERVICE_SERVER_DRIVEN)
    {
        PRINT_DATA(" SINGLE_SERVICE_SERVER_DRIVEN ");
    }
    if (dpom & SINGLE_SERVICE_CLIENT_DRIVEN)
    {
        PRINT_DATA(" SINGLE_SERVICE_CLIENT_DRIVEN ");
    }
    PRINT_DATA(") \n");
}

#ifdef MULTIPLE_OWNER
static void PrintMom(const OicSecMom_t *mom)
{
    if (mom)
    {
        PRINT_DATA("%d (", mom->mode);

        switch (mom->mode)
        {
            case OIC_MULTIPLE_OWNER_DISABLE:
                PRINT_DATA(" OIC_MULTIPLE_OWNER_DISABLE ");
                break;
            case OIC_MULTIPLE_OWNER_ENABLE:
                PRINT_DATA(" OIC_MULTIPLE_OWNER_ENABLE ");
                break;
            case OIC_MULTIPLE_OWNER_TIMELY_ENABLE:
                PRINT_DATA(" OIC_MULTIPLE_OWNER_TIMELY_ENABLE ");
                break;
            default:
                break;
        }

        PRINT_DATA(") \n");
    }
    else
    {
        PRINT_DATA("NULL\n");
    }
}
#endif
static void PrintCredType(OicSecCredType_t credType)
{
    PRINT_DATA("%d", credType);
    switch (credType)
    {
        case NO_SECURITY_MODE:
            PRINT_DATA(" (NO_SECURITY_MODE)\n");
            break;
        case SYMMETRIC_PAIR_WISE_KEY:
            PRINT_DATA(" (SYMMETRIC_PAIR_WISE_KEY)\n");
            break;
        case SYMMETRIC_GROUP_KEY:
            PRINT_DATA(" (SYMMETRIC_GROUP_KEY)\n");
            break;
        case ASYMMETRIC_KEY:
            PRINT_DATA(" (ASYMMETRIC_KEY)\n");
            break;
        case SIGNED_ASYMMETRIC_KEY:
            PRINT_DATA(" (SIGNED_ASYMMETRIC_KEY)\n");
            break;
        case PIN_PASSWORD:
            PRINT_DATA(" (PIN_PASSWORD)\n");
            break;
        case ASYMMETRIC_ENCRYPTION_KEY:
            PRINT_DATA(" (ASYMMETRIC_ENCRYPTION_KEY)\n");
            break;
        default:
            PRINT_ERR(" (Unknown Cred type)");
            break;
    }
}

static void PrintCredEncodingType(OicEncodingType_t encoding)
{
    PRINT_DATA("%d", encoding);
    switch (encoding)
    {
        case OIC_ENCODING_RAW:
            PRINT_DATA(" (OIC_ENCODING_RAW)\n");
            break;
        case OIC_ENCODING_BASE64:
            PRINT_DATA(" (OIC_ENCODING_BASE64)\n");
            break;
        case OIC_ENCODING_PEM:
            PRINT_DATA(" (OIC_ENCODING_PEM)\n");
            break;
        case OIC_ENCODING_DER:
            PRINT_DATA(" (OIC_ENCODING_DER)\n");
            break;
        default:
            PRINT_ERR(" (Unknown Encoding type)");
            break;
    }

}

static void PrintHelp()
{
    PRINT_ERR("<This program requires one input>");
    PRINT_INFO("./svrdbeditor <svr_db_file_path>");
}

static void PrintDoxm(const OicSecDoxm_t *doxm)
{
    PRINT_INFO("\n\n********************* [%-20s] *********************",
               "DOXM Resource");

    PRINT_PROG("%15s : ", OIC_JSON_OWNED_NAME);
    (doxm->owned ? PrintString("True (Owned)") : PrintString("False (Unowned)"));

    PRINT_PROG("%15s : ", OIC_JSON_OXMS_NAME);
    PrintIntArray((int *)doxm->oxm, doxm->oxmLen);

    PRINT_PROG("%15s : ", OIC_JSON_OXM_SEL_NAME);
    PrintInt((int)doxm->oxmSel);

    PRINT_PROG("%15s : ", OIC_JSON_SUPPORTED_CRED_TYPE_NAME);
    PrintInt((int)doxm->sct);

#ifdef MULTIPLE_OWNER
    PRINT_PROG("%15s : ", OIC_JSON_MOM_NAME);
    PrintMom(doxm->mom);

    // TODO: Print Subowner List
#endif //MULTIPLE_OWNER

    PRINT_PROG("%15s : ", OIC_JSON_DEVICE_ID_NAME);
    PrintUuid(&doxm->deviceID);

    PRINT_PROG("%15s : ", OIC_JSON_DEVOWNERID_NAME);
    PrintUuid(&doxm->owner);

    PRINT_PROG("%15s : ", OIC_JSON_ROWNERID_NAME);
    PrintUuid(&doxm->rownerID);
    PRINT_INFO("********************* [%-20s] *********************",
               "DOXM Resource");
}

static void PrintPstat(const OicSecPstat_t *pstat)
{
    PRINT_INFO("\n\n********************* [%-20s] *********************",
               "PSTAT Resource");
    PRINT_PROG("%15s : ", OIC_JSON_ISOP_NAME);
    (pstat->isOp ? PrintString("True") : PrintString("False"));

    PRINT_PROG("%15s : ", OIC_JSON_SM_NAME);
    for (size_t i = 0; i < pstat->smLen; i++)
    {
        PrintDpom(pstat->sm[i]);
    }

    PRINT_PROG("%15s : ", OIC_JSON_OM_NAME);
    PrintDpom(pstat->om);

    PRINT_PROG("%15s : ", OIC_JSON_CM_NAME);
    PrintDpm(pstat->cm);

    PRINT_PROG("%15s : ", OIC_JSON_TM_NAME);
    PrintDpm(pstat->tm);

    PRINT_PROG("%15s : ", OIC_JSON_ROWNERID_NAME);
    PrintUuid(&pstat->rownerID);
    PRINT_INFO("********************* [%-20s] *********************",
               "PSTAT Resource");
}

static void PrintResourceList(const OicSecRsrc_t *rsrcList)
{
    const OicSecRsrc_t *rsrc = NULL;
    const OicSecRsrc_t *tempRsrc = NULL;
    size_t rsrcCnt = 0;

    LL_FOREACH_SAFE(rsrcList, rsrc, tempRsrc)
    {
        PRINT_DATA("Resource #%zu:\n", rsrcCnt + 1);
        PRINT_DATA("%10s : %s\n", OIC_JSON_HREF_NAME, rsrc->href);
        PRINT_DATA("%10s : %s\n", OIC_JSON_REL_NAME, rsrc->rel);
        PRINT_DATA("%10s : ", OIC_JSON_RT_NAME);
        PrintStringArray((const char **)rsrc->types, rsrc->typeLen);
        PRINT_DATA("%10s : ", OIC_JSON_IF_NAME);
        PrintStringArray((const char **)rsrc->interfaces, rsrc->interfaceLen);
        rsrcCnt++;
    }
}

static void PrintValidity(const OicSecValidity_t *validities)
{
    const OicSecValidity_t *validity = NULL;
    const OicSecValidity_t *tempValidity = NULL;
    size_t validityCnt = 0;

    LL_FOREACH_SAFE(validities, validity, tempValidity)
    {
        PRINT_DATA("Validity #%zu:\n", validityCnt + 1);
        PRINT_DATA("%10s : %s\n", OIC_JSON_PERIOD_NAME, validity->period);
        PRINT_DATA("%10s : ", OIC_JSON_RESOURCES_NAME);
        PrintStringArray((const char **)validity->recurrences, validity->recurrenceLen);
        validityCnt++;
    }
}

static void PrintPermission(uint16_t permission)
{
    PRINT_DATA("%d (", permission);

    if (0 == permission)
    {
        PRINT_DATA(" NO PERMISSION");
    }
    else
    {
        if (permission & PERMISSION_CREATE)
        {
            PRINT_DATA(" CREATE ");
        }
        if (permission & PERMISSION_READ)
        {
            PRINT_DATA(" READ ");
        }
        if (permission & PERMISSION_WRITE)
        {
            PRINT_DATA(" WRITE ");
        }
        if (permission & PERMISSION_DELETE)
        {
            PRINT_DATA(" DELETE ");
        }
        if (permission & PERMISSION_NOTIFY)
        {
            PRINT_DATA(" NOTIFY ");
        }
    }

    PRINT_DATA(") \n");
}

static size_t PrintAcl(const OicSecAcl_t *acl)
{
    char *strUuid = NULL;

    OicSecAce_t *ace = NULL;
    OicSecAce_t *tempAce = NULL;
    bool isEmptyList = true;
    size_t aceCnt = 0;

    PRINT_INFO("\n\n********************* [%-20s] *********************",
               "ACL Resource");

    if (acl)
    {
        LL_FOREACH_SAFE(acl->aces, ace, tempAce)
        {
            PRINT_INFO("[ACE #%zu]", ++aceCnt);
            PRINT_PROG("%15s : ", OIC_JSON_SUBJECTID_NAME);
            if (memcmp(&(ace->subjectuuid), &WILDCARD_SUBJECT_ID, sizeof(OicUuid_t)) == 0)
            {
                PrintString((char *)WILDCARD_SUBJECT_ID.id);
            }
            else
            {
                strUuid = NULL;
                if (OC_STACK_OK != ConvertUuidToStr(&(ace->subjectuuid), &strUuid))
                {
                    PRINT_ERR("Failed ConvertUuidToStr");
                    return aceCnt;
                }
                PrintString(strUuid);
                OICFree(strUuid);
            }

#ifdef MULTIPLE_OWNER
            if (ace->eownerID)
            {
                PRINT_PROG("%15s : ", OIC_JSON_EOWNERID_NAME);
                strUuid = NULL;
                if (OC_STACK_OK != ConvertUuidToStr(ace->eownerID, &strUuid))
                {
                    PRINT_ERR("Failed ConvertUuidToStr");
                    return aceCnt;
                }
                PrintString(strUuid);
                OICFree(strUuid);
            }
#endif

            //permission
            PRINT_PROG("%15s : ", OIC_JSON_PERMISSION_NAME);
            PrintPermission(ace->permission);

            //resource list
            PRINT_PROG("%15s : \n", OIC_JSON_RESOURCES_NAME);
            PrintResourceList(ace->resources);

            //Validity
            PrintValidity(ace->validities);

            PRINT_PROG("------------------------------------------------------------------\n");
            isEmptyList = false;
        }

        if (isEmptyList)
        {
            PRINT_PROG("ACE is empty.\n");
            PRINT_PROG("------------------------------------------------------------------\n");
        }

        PRINT_PROG("%15s : ", OIC_JSON_ROWNERID_NAME);
        strUuid = NULL;
        if (OC_STACK_OK != ConvertUuidToStr(&(acl->rownerID), &strUuid))
        {
            PRINT_ERR("Failed ConvertUuidToStr");
            return aceCnt;
        }
        PrintString(strUuid);
        OICFree(strUuid);
    }
    else
    {
        PRINT_PROG("ACL is empty.\n");
    }

    PRINT_INFO("********************* [%-20s] *********************",
               "ACL Resource");

    return aceCnt;
}

/**
 * This API to print credential list.
 * Also return the number of credential in credential list.
 */
static void PrintCredList(const OicSecCred_t *creds)
{
    const OicSecCred_t *cred = NULL;
    const OicSecCred_t *tempCred = NULL;
    bool isEmptyList = true;
    char *strUuid = NULL;
    PRINT_INFO("\n\n********************* [%-20s] *********************",
               "Credential Resource");
    LL_FOREACH_SAFE(creds, cred, tempCred)
    {
        PRINT_PROG("%15s : ", OIC_JSON_CREDID_NAME);
        PrintInt(cred->credId);

        PRINT_PROG("%15s : ", OIC_JSON_SUBJECTID_NAME);
        if (memcmp(&(cred->subject), &WILDCARD_SUBJECT_ID, sizeof(OicUuid_t)) == 0)
        {
            PrintString((char *)WILDCARD_SUBJECT_ID.id);
        }
        else
        {
            strUuid = NULL;
            if (OC_STACK_OK != ConvertUuidToStr(&(cred->subject), &strUuid))
            {
                PRINT_ERR("Failed ConvertUuidToStr");
                return;
            }
            PrintString(strUuid);
            OICFree(strUuid);
        }

#ifdef MULTIPLE_OWNER
        if (creds->eownerID)
        {
            PRINT_PROG("%15s : ", OIC_JSON_EOWNERID_NAME);
            strUuid = NULL;
            if (OC_STACK_OK != ConvertUuidToStr(cred->eownerID, &strUuid))
            {
                PRINT_ERR("Failed ConvertUuidToStr");
                return;
            }
            PrintString(strUuid);
            OICFree(strUuid);
        }
#endif

        PRINT_PROG("%15s : ", OIC_JSON_CREDTYPE_NAME);
        PrintCredType(cred->credType);

        switch (cred->credType)
        {
            case SYMMETRIC_PAIR_WISE_KEY:
            case SYMMETRIC_GROUP_KEY:
                PRINT_PROG("%15s : \n", OIC_JSON_PRIVATEDATA_NAME);
                if (cred->privateData.data)
                {
                    PRINT_DATA("%s : ", OIC_JSON_ENCODING_NAME);
                    PrintCredEncodingType(cred->privateData.encoding);

                    PRINT_DATA("%s : ", OIC_JSON_DATA_NAME);
                    if (OIC_ENCODING_BASE64 == cred->privateData.encoding)
                    {
                        PrintString((char *)cred->privateData.data);
                    }
                    else
                    {
                        PrintBuffer(cred->privateData.data, cred->privateData.len);
                    }
                }
                else
                {
                    PRINT_ERR("Private data is null");
                }
                break;
            case ASYMMETRIC_KEY:
            case SIGNED_ASYMMETRIC_KEY:
                // TODO: Print certificate and asymmetric key in readable formats

                //cred usage
                if (cred->credUsage)
                {
                    PRINT_PROG("%15s : ", OIC_JSON_CREDUSAGE_NAME);
                    PRINT_DATA("%s\n", cred->credUsage);
                }

                //private data
                if (cred->privateData.data)
                {
                    PRINT_PROG("%15s : ", OIC_JSON_PRIVATEDATA_NAME);
                    PRINT_INFO("will be updated to print private data");

                    PrintBuffer(cred->privateData.data, cred->privateData.len);

                    if (cred->credUsage &&
                        (strcmp(cred->credUsage, PRIMARY_CERT) == 0 ||
                         strcmp(cred->credUsage, MF_PRIMARY_CERT) == 0))
                    {
                        // TODO: T.B.D
                    }
                    else
                    {
                        // TODO: T.B.D
                    }
                }

                //public data
                if (cred->publicData.data)
                {
                    PRINT_PROG("%15s : ", OIC_JSON_PUBLICDATA_NAME);
                    PRINT_DATA("%-17s : ", OIC_JSON_ENCODING_NAME);
                    PrintCredEncodingType(cred->publicData.encoding);


                    if (cred->credUsage &&
                        (strcmp(cred->credUsage, PRIMARY_CERT) == 0 ||
                         strcmp(cred->credUsage, MF_PRIMARY_CERT) == 0))
                    {
                        char buf[2048];
                        mbedtls_x509_crt crt;
                        mbedtls_x509_crt *tmpCrt = NULL;
                        PkiInfo_t inf;
                        int i = 0;

                        memset(&inf, 0x00, sizeof(PkiInfo_t));
                        mbedtls_x509_crt_init(&crt);

                        ParseDerOwnCert(&inf.crt, cred->credUsage, cred->credId);
                        ParseCertChain(&crt, inf.crt.data, inf.crt.len);

                        for (i = 0, tmpCrt = &crt; NULL != tmpCrt; i++, tmpCrt = tmpCrt->next)
                        {
                            PRINT_INFO("[Cert #%d]", (i + 1));
                            mbedtls_x509_crt_info( buf, sizeof(buf) - 1, "", tmpCrt );
                            PRINT_DATA("%s", buf);
                        }
                        mbedtls_x509_crt_free(&crt);
                    }
                    else
                    {
                        PRINT_INFO("will be updated to print public data");
                    }
                }

                //optional data
                if (cred->optionalData.data)
                {
                    PRINT_PROG("%15s : \n", OIC_JSON_OPTDATA_NAME);

                    //revocation status
                    PRINT_DATA("%-17s : %s\n", OIC_JSON_REVOCATION_STATUS_NAME,
                               (cred->optionalData.revstat ? "True" : "False"));

                    PRINT_DATA("%-17s : ", OIC_JSON_ENCODING_NAME);
                    PrintCredEncodingType(cred->optionalData.encoding);

                    //CA chain
                    if (cred->credUsage &&
                        (strcmp(cred->credUsage, TRUST_CA) == 0 ||
                         strcmp(cred->credUsage, MF_TRUST_CA) == 0))
                    {
                        char buf[2048];
                        mbedtls_x509_crt ca;
                        mbedtls_x509_crt *tmpCa = NULL;
                        PkiInfo_t inf;
                        int i = 0;

                        memset(&inf, 0x00, sizeof(PkiInfo_t));
                        mbedtls_x509_crt_init(&ca);

                        ParseDerCaCert(&inf.ca, cred->credUsage, cred->credId);
                        ParseCertChain(&ca, inf.ca.data, inf.ca.len);

                        for (i = 0, tmpCa = &ca; NULL != tmpCa; i++, tmpCa = tmpCa->next)
                        {
                            PRINT_INFO("[Cert #%d]", (i + 1));
                            mbedtls_x509_crt_info( buf, sizeof(buf) - 1, "", tmpCa );
                            PRINT_DATA("%s", buf);
                        }
                        mbedtls_x509_crt_free(&ca);
                    }
                    else
                    {
                        // TODO: T.B.D
                        PRINT_INFO("will be updated to print optional data");
                    }
                }
                break;
            case PIN_PASSWORD:
                PRINT_PROG("%15s : ", OIC_JSON_PRIVATEDATA_NAME);
                if (cred->privateData.data)
                {
                    PRINT_DATA("%s : ", OIC_JSON_ENCODING_NAME);
                    PrintCredEncodingType(cred->privateData.encoding);

                    PRINT_DATA("%s : ", OIC_JSON_DATA_NAME);
                    PRINT_DATA("%s\n", cred->privateData.data);
                }
                else
                {
                    PRINT_ERR("Private data is null");
                }
                break;
            case ASYMMETRIC_ENCRYPTION_KEY:
                break;
            default:
                PRINT_ERR(" (Unknown Cred type)");
                break;
        }
        PRINT_PROG("------------------------------------------------------------------\n");
        isEmptyList = false;
    }

    if (!isEmptyList)
    {
        PRINT_PROG("%15s : ", OIC_JSON_ROWNERID_NAME);
        strUuid = NULL;
        if (OC_STACK_OK != ConvertUuidToStr(&(creds->rownerID), &strUuid))
        {
            PRINT_ERR("Failed ConvertUuidToStr");
            return;
        }
        PrintString(strUuid);
        OICFree(strUuid);
    }
    else
    {
        PRINT_PROG("Cred List is empty.\n");
    }

    PRINT_INFO("********************* [%-20s] *********************",
               "Credential Resource");

    return;
}

static int InputNumber(const char *infoText)
{
    int inputValue = 0;

    PRINT_PROG("%s", infoText);
    for (int ret = 0; 1 != ret; )
    {
        ret = scanf("%d", &inputValue);
        for ( ; 0x20 <= getchar(); ); // for removing overflow garbages
        // '0x20<=code' is character region
    }

    return inputValue;
}

inline static char InputChar(const char *infoText)
{
    char inputValue = 0;

    PRINT_PROG("%s", infoText);
    for (int ret = 0; 1 != ret; )
    {
        ret = scanf("%c", &inputValue);
        for ( ; 0x20 <= getchar(); ); // for removing overflow garbages
        // '0x20<=code' is character region
    }

    return inputValue;
}

static char *InputString(const char *infoText)
{
    char tmpStr[SVR_DB_PATH_LENGTH] = {0};

    PRINT_PROG("%s", infoText);
    for (int ret = 0; 1 != ret; )
    {
        ret = scanf("%1024s", tmpStr);
        for ( ; 0x20 <= getchar(); ); // for removing overflow garbages
        // '0x20<=code' is character region
    }

    return OICStrdup(tmpStr);
}

static void PrintEditMenu(const char *resourceName, bool print, bool add, bool remove, bool modify)
{
    PRINT_PROG("\n\nYou can perform the "
               CYAN_BEGIN "cyan color opertions " COLOR_END
               BOLD_BEGIN "for" COLOR_END
               YELLOW_BEGIN " %s" COLOR_END_NL, resourceName);

    for (int i = 0; i < SVR_EDIT_IDX_SIZE; i++)
    {
        g_allowedEditMenu[i] = false;
    }

    if (print)
    {
        g_allowedEditMenu[SVR_PRINT] = true;
        PRINT_DATA("\t%2d. Print all entities\n", SVR_PRINT);
    }
    else
    {
        PRINT_NORMAL("\t%2d. Print all entities\n", SVR_PRINT);
    }

    if (add)
    {
        g_allowedEditMenu[SVR_ADD] = true;
        PRINT_DATA("\t%2d. Add entity\n", SVR_ADD);
    }
    else
    {
        PRINT_NORMAL("\t%2d. Add entity\n", SVR_ADD);
    }


    if (remove)
    {
        g_allowedEditMenu[SVR_REMOVE] = true;
        PRINT_DATA("\t%2d. Remove entity\n", SVR_REMOVE);
    }
    else
    {
        PRINT_NORMAL("\t%2d. Remove entity\n", SVR_REMOVE);
    }

    if (modify)
    {
        g_allowedEditMenu[SVR_MODIFY] = true;
        PRINT_DATA("\t%2d. Modify entity\n", SVR_MODIFY);
    }
    else
    {
        PRINT_NORMAL("\t%2d. Modify entity\n", SVR_MODIFY);
    }
    PRINT_DATA("\t%2d. Back to the main menu\n", BACK);
}

static void PrintMainMenu()
{
    PRINT_PROG("\n\nYou can perform the "
               CYAN_BEGIN "cyan color opertions : " COLOR_END_NL);

    PRINT_DATA("\t%2d. Print All Security Resource.\n", SVR_PRINT_ALL);
    PRINT_DATA("\t%2d. Edit Credential Resource.\n", SVR_EDIT_CRED);
    PRINT_DATA("\t%2d. Edit ACL Resource.\n", SVR_EDIT_ACL);
    PRINT_PROG("\t%2d. Edit Doxm Resource. (T.B.D)\n", SVR_EDIT_DOXM);
    PRINT_PROG("\t%2d. Edit Pstat Resource. (T.B.D)\n", SVR_EDIT_PSTAT);
    PRINT_DATA("\t%2d. Exit.\n", EXIT);
}

static int InputUuid(OicUuid_t *uuid)
{
    char strSubject[UUID_LENGTH * 2 + 4 + 1] = {0};
    OCStackResult ocResult = OC_STACK_ERROR;

    if (NULL == uuid)
    {
        PRINT_ERR("Failed ConvertStrToUuid");
        return -1;
    }

    for (int ret = 0; 1 != ret; )
    {
        ret = scanf("%37s", strSubject);
        for ( ; 0x20 <= getchar(); ); // for removing overflow garbages
        // '0x20<=code' is character region
    }

    if (strncmp(strSubject, (char *)WILDCARD_SUBJECT_ID.id, sizeof(OicUuid_t)) == 0)
    {
        memset(uuid->id, 0x00, sizeof(uuid->id));
        memcpy(uuid->id, WILDCARD_SUBJECT_ID.id, WILDCARD_SUBJECT_ID_LEN);
    }
    else
    {
        ocResult = ConvertStrToUuid(strSubject, uuid);
        if (OC_STACK_OK != ocResult)
        {
            PRINT_ERR("Failed ConvertStrToUuid");
            return -1;
        }
    }

    return 0;
}

static int InputResources(OicSecRsrc_t *resources)
{
    size_t i = 0;

    if (NULL == resources)
    {
        PRINT_ERR("InputResources : Invalid param");
        return -1;
    }

    memset(resources, 0x00, sizeof(OicSecRsrc_t));

    resources->href = InputString("\tInput the resource URI : ");
    if (NULL == resources->href)
    {
        PRINT_ERR("InputResources : Failed InputString");
        return -1;
    }

    PRINT_PROG("\tInput the number of interface for %s : ", resources->href);
    resources->interfaceLen = InputNumber("");
    if (0 == resources->interfaceLen || SVR_MAX_ENTITY < resources->interfaceLen)
    {
        PRINT_ERR("Invalid number");
        return -1;
    }

    resources->interfaces = (char **)OICMalloc(sizeof(char **) * resources->interfaceLen);
    if (NULL == resources->interfaces)
    {
        PRINT_ERR("InputResources : Failed to allocate memory");
        return -1;
    }

    for (i = 0; i < resources->interfaceLen; i++)
    {
        PRINT_PROG("\tInput the interface name #%zu : ", i + 1);
        resources->interfaces[i] = InputString("");
        if (NULL == resources->interfaces[i] )
        {
            PRINT_ERR("InputResources : Failed InputString");
            return -1;
        }
    }


    PRINT_PROG("\tInput the number of resource type for %s : ", resources->href);
    resources->typeLen = InputNumber("");
    if (0 == resources->typeLen || SVR_MAX_ENTITY < resources->typeLen)
    {
        PRINT_ERR("Invalid number");
        return -1;
    }

    resources->types = (char **)OICMalloc(sizeof(char **) * resources->typeLen);
    if (NULL == resources->types)
    {
        PRINT_ERR("InputResources : Failed to allocate memory");
        return -1;
    }

    for (i = 0; i < resources->typeLen; i++)
    {
        PRINT_PROG("\tInput the resource type name #%zu : ", i + 1);
        resources->types[i] = InputString("");
        if (NULL == resources->types[i] )
        {
            PRINT_ERR("InputResources : Failed InputString");
            return -1;
        }
    }

    return 0;
}

static uint16_t InputAccessPermission()
{
    uint16_t pmsn = PERMISSION_FULL_CONTROL;  // default full permission
    uint16_t pmsn_msk = PERMISSION_CREATE;  // default permission mask
    const char *ACL_PEMISN[ACL_PEMISN_CNT] = {"CREATE", "READ", "WRITE", "DELETE", "NOTIFY"};

    for (int i = 0; i < ACL_PEMISN_CNT; i++)
    {
        char ans = 0;
        for ( ; ; )
        {
            PRINT_NORMAL("\tEnter %s Permission (y/n): ", ACL_PEMISN[i]);
            for (int ret = 0; 1 != ret; )
            {
                ret = scanf("%c", &ans);
                for ( ; 0x20 <= getchar(); ); // for removing overflow garbages
                // '0x20<=code' is character region
            }
            if ('y' == ans || 'Y' == ans || 'n' == ans || 'N' == ans)
            {
                ans &= ~0x20;  // for masking lower case, 'y/n'
                break;
            }
            PRINT_NORMAL("\tEntered Wrong Answer. Please Enter 'y/n' Again\n");
        }
        if ('N' == ans)  // masked lower case, 'n'
        {
            pmsn -= pmsn_msk;
        }
        pmsn_msk <<= 1;
    }
    return pmsn;
}

static int InputAceData(OicSecAce_t *ace)
{
    int ret = 0;
    size_t numOfRsrc = 0;

    PRINT_PROG("\n\nPlease input the each entity of new ACE.\n");

    PRINT_PROG(
        "\tInput the Subject UUID for this access (e.g. 61646D69-6E44-6576-6963-655575696430) : ");
    ret = InputUuid(&ace->subjectuuid);
    if (0 != ret)
    {
        PRINT_ERR("InputAceData : Failed InputUuid");
        return ret;
    }

    PRINT_PROG("\tInput the number of resource for this access : ");
    numOfRsrc = InputNumber("");
    if (0 == numOfRsrc || SVR_MAX_ENTITY < numOfRsrc)
    {
        PRINT_ERR("Invalid number");
        return -1;
    }

    for (size_t i = 0; i < numOfRsrc; i++)
    {
        PRINT_PROG("Please input the resource information for resource #%zu\n", i + 1);
        OicSecRsrc_t *rsrc = (OicSecRsrc_t *)OICCalloc(1, sizeof(OicSecRsrc_t));
        if (NULL == rsrc)
        {
            PRINT_ERR("InputAceData : Failed to allocate memory");
            return -1;
        }
        LL_APPEND(ace->resources, rsrc);

        ret = InputResources(rsrc);
        if (0 != ret)
        {
            PRINT_ERR("InputAceData : Failed InputResources");
            return ret;
        }
    }

    PRINT_PROG("\tSelect permission for this access.\n");
    ace->permission = InputAccessPermission();

#ifdef MULTIPLE_OWNER
    // TODO: Input eowner
#endif

    // TODO: Input the validity (T.B.D)

    return 0;
}

static int InputCredUsage(char **credUsage)
{
    char inputUsage[128] = {0};
    int credUsageNum = 0;

    if (NULL == credUsage || NULL != *credUsage)
    {
        PRINT_ERR("InputCredUsage error : invaild param");
        return -1;
    }

    do
    {
        PRINT_NORMAL("\n\n");
        PRINT_NORMAL("\t1. %s\n", TRUST_CA);
        PRINT_NORMAL("\t2. %s\n", PRIMARY_CERT);
        PRINT_NORMAL("\t3. %s\n", MF_TRUST_CA);
        PRINT_NORMAL("\t4. %s\n", MF_PRIMARY_CERT);
        PRINT_NORMAL("\t5. Input manually\n");
        credUsageNum = InputNumber("\tSelect the credential usage : ");
        switch (credUsageNum)
        {
            case 1:
                *credUsage = OICStrdup(TRUST_CA);
                break;
            case 2:
                *credUsage = OICStrdup(PRIMARY_CERT);
                break;
            case 3:
                *credUsage = OICStrdup(MF_TRUST_CA);
                break;
            case 4:
                *credUsage = OICStrdup(MF_PRIMARY_CERT);
                break;
            case 5:
                PRINT_NORMAL("\tInput the credential usage : ");
                for (int ret = 0; 1 != ret; )
                {
                    ret = scanf("%128s", inputUsage);
                    for ( ; 0x20 <= getchar(); ); // for removing overflow garbages
                    // '0x20<=code' is character region
                }
                *credUsage = OICStrdup(inputUsage);
                break;
            default:
                PRINT_ERR("Invaild credential usage");
                credUsageNum = 0;
                break;
        }
    }
    while (0 == credUsageNum);

    if (NULL == *credUsage)
    {
        PRINT_ERR("Failed OICStrdup");
        return -1;
    }

    return 0;
}

static int InputCredEncodingType(const char *dataType, OicEncodingType_t *encoding)
{
    int credEncType = 0;
    char infoText[512] = {0};

    if (NULL == dataType || NULL == encoding)
    {
        PRINT_ERR("InputCredEncodingType : Invaild param");
        return -1;
    }

    snprintf(infoText, sizeof(infoText), "\tSelect the encoding type of %s : ", dataType);

    do
    {
        PRINT_NORMAL("\n\n");
        PRINT_NORMAL("\t%d. %s\n", OIC_ENCODING_RAW, "OIC_ENCODING_RAW");
        PRINT_NORMAL("\t%d. %s\n", OIC_ENCODING_BASE64, "OIC_ENCODING_BASE64");
        PRINT_NORMAL("\t%d. %s\n", OIC_ENCODING_PEM, "OIC_ENCODING_PEM");
        PRINT_NORMAL("\t%d. %s\n", OIC_ENCODING_DER, "OIC_ENCODING_DER");
        credEncType = InputNumber(infoText);
        switch ( (OicEncodingType_t)credEncType )
        {
            case OIC_ENCODING_RAW:
                break;
            case OIC_ENCODING_BASE64:
                break;
            case OIC_ENCODING_PEM:
                break;
            case OIC_ENCODING_DER:
                break;
            default:
                PRINT_ERR("Invaild encoding type");
                credEncType = 0;
                break;
        }
    }
    while (0 == credEncType);

    *encoding = (OicEncodingType_t)credEncType;

    return 0;
}

static int ReadDataFromFile(const char *infoTxt, uint8_t **buffer, size_t *bufferSize)
{
    char filePath[512] = {0};
    char tmpBuffer[SVR_DB_PATH_LENGTH] = {0};
    FILE *fp = NULL;
    size_t fileSize = 0;

    if (NULL == buffer || NULL != *buffer || NULL == bufferSize)
    {
        PRINT_ERR("ReadDataFromFile : Invaild param");
        return -1;
    }

    PRINT_NORMAL("%s", infoTxt);
    for (int ret = 0; 1 != ret; )
    {
        ret = scanf("%512s", filePath);
        for ( ; 0x20 <= getchar(); ); // for removing overflow garbages
        // '0x20<=code' is character region
    }

    //Get a file size
    fp = fopen(filePath, "rb");
    if (fp)
    {
        size_t bytesRead = 0;
        do
        {
            bytesRead = fread(tmpBuffer, 1, 1023, fp);
            fileSize += bytesRead;
        }
        while (bytesRead);
        fclose(fp);
        fp = NULL;
    }
    else
    {
        PRINT_ERR("Failed to open %s" , filePath);
        PRINT_ERR("Please make sure the file path and access permission.");
        goto error;
    }

    if (0 == fileSize)
    {
        PRINT_ERR("%s is empty." , filePath);
        goto error;
    }

    fp = fopen(filePath, "rb");
    if (fp)
    {
        *buffer = (uint8_t *) OICCalloc(1, fileSize);
        if ( NULL == *buffer)
        {
            PRINT_ERR("Failed to allocate memory.");
            goto error;
        }

        if ( fread(*buffer, 1, fileSize, fp) == fileSize)
        {
            *bufferSize = fileSize;
        }
        fclose(fp);
    }
    else
    {
        PRINT_ERR("Failed to open %s" , filePath);
        PRINT_ERR("Please make sure the file path and access permission.");
        goto error;
    }

    return 0;

error:
    if (fp)
    {
        fclose(fp);
    }
    if (*buffer)
    {
        OICFree(*buffer);
    }
    return -1;
}

static int InputCredentialData(OicSecCred_t *cred)
{
    uint8_t *certChain = NULL;
    uint8_t *privateKey = NULL;
    uint8_t *publicKey = NULL;
    size_t certChainSize = 0;
    size_t privateKeySize = 0;
    size_t publicKeySize = 0;


    PRINT_PROG("\n\nPlease input the each entity of new credential.\n");

    PRINT_NORMAL("\t%3d. Symmetric pair wise key\n", SYMMETRIC_PAIR_WISE_KEY);
    PRINT_NORMAL("\t%3d. Symmetric group key\n", SYMMETRIC_GROUP_KEY);
    PRINT_NORMAL("\t%3d. Asymmetric key\n", ASYMMETRIC_KEY);
    PRINT_NORMAL("\t%3d. Signed asymmetric key\n", SIGNED_ASYMMETRIC_KEY);
    PRINT_NORMAL("\t%3d. PIN/Password\n", PIN_PASSWORD);
    PRINT_NORMAL("\t%3d. Asymmetric encryption key\n", ASYMMETRIC_ENCRYPTION_KEY);
    cred->credType = (OicSecCredType_t)InputNumber("\tSelect the credential type : ");
    if (SYMMETRIC_PAIR_WISE_KEY != cred->credType &&
        SYMMETRIC_GROUP_KEY != cred->credType &&
        SIGNED_ASYMMETRIC_KEY != cred->credType &&
        PIN_PASSWORD != cred->credType &&
        ASYMMETRIC_ENCRYPTION_KEY != cred->credType)
    {
        PRINT_ERR("Invaild credential type");
        goto error;
    }

    //Input the key data according to credential type
    switch (cred->credType)
    {
        case SYMMETRIC_PAIR_WISE_KEY:
            PRINT_INFO("Unsupported yet.");
            goto error;
            // TODO: T.B.D
            /*
            PRINT_PROG("\tSubject UUID (e.g. 61646D69-6E44-6576-6963-655575696430) : ");
            if (0 != InputUuid(&cred->subject))
            {
                PRINT_ERR("InputUuid error");
                goto error;
            }
            */
            break;
        case SYMMETRIC_GROUP_KEY:
            // TODO: T.B.D
            PRINT_INFO("Unsupported yet.");
            goto error;
            break;
        case ASYMMETRIC_KEY:
            // TODO: T.B.D
            PRINT_INFO("Unsupported yet.");
            goto error;
            break;
        case SIGNED_ASYMMETRIC_KEY:
            //Credential usage
            if ( 0 != InputCredUsage(&cred->credUsage))
            {
                PRINT_ERR("Failed InputCredUsage");
                goto error;
            }

            //Input the other data according to credential usage.
            if ( strcmp(cred->credUsage, TRUST_CA) == 0 ||
                 strcmp(cred->credUsage, MF_TRUST_CA) == 0)
            {
                //Subject
                memcpy(cred->subject.id, g_doxmResource->deviceID.id, sizeof(g_doxmResource->deviceID.id));

                //encoding type
                if ( 0 != InputCredEncodingType("certificate chain", &cred->optionalData.encoding))
                {
                    PRINT_ERR("Failed InputCredEncodingType");
                    goto error;
                }

                //Read chain data from file (readed data will be saved to optional data)
                if (0 != ReadDataFromFile("\tInput the certificate chain path : ", &certChain, &certChainSize))
                {
                    PRINT_ERR("Failed ReadDataFromFile");
                    goto error;
                }

                //optional data
                if (cred->optionalData.encoding == OIC_ENCODING_PEM)
                {
                    cred->optionalData.data = (uint8_t *)OICCalloc(1, certChainSize + 1);
                    if (NULL == cred->optionalData.data)
                    {
                        PRINT_ERR("InputCredentialData : Failed to allocate memory.");
                        goto error;
                    }
                    cred->optionalData.len = certChainSize + 1;
                }
                else
                {
                    cred->optionalData.data = (uint8_t *)OICCalloc(1, certChainSize);
                    if (NULL == cred->optionalData.data)
                    {
                        PRINT_ERR("InputCredentialData : Failed to allocate memory.");
                        goto error;
                    }
                    cred->optionalData.len = certChainSize;
                }
                memcpy(cred->optionalData.data, certChain, certChainSize);
                cred->optionalData.revstat = false;
            }
            else if ( strcmp(cred->credUsage, PRIMARY_CERT) == 0 ||
                      strcmp(cred->credUsage, MF_PRIMARY_CERT) == 0)
            {
                memcpy(cred->subject.id, g_doxmResource->deviceID.id, sizeof(g_doxmResource->deviceID.id));

                //private key
                //encoding type
                if ( 0 != InputCredEncodingType(YELLOW_BEGIN"Private key"COLOR_END, &cred->privateData.encoding))
                {
                    PRINT_ERR("Failed InputCredEncodingType");
                    goto error;
                }

                if (OIC_ENCODING_RAW != cred->privateData.encoding)
                {
                    PRINT_ERR("Unsupported encoding type for private key");
                    goto error;
                }

                //Read private key data from file
                if (0 != ReadDataFromFile("\tInput the private key's path : ", &privateKey, &privateKeySize))
                {
                    PRINT_ERR("Failed ReadDataFromFile");
                    goto error;
                }

                cred->privateData.data = OICCalloc(1, privateKeySize);
                if (NULL == cred->privateData.data)
                {
                    PRINT_ERR("InputCredentialData : Failed to allocate memory.");
                    goto error;
                }
                memcpy(cred->privateData.data, privateKey, privateKeySize);
                cred->privateData.len = privateKeySize;


                //public key
                //encoding type
                if ( 0 != InputCredEncodingType(YELLOW_BEGIN"Certificate"COLOR_END, &cred->publicData.encoding))
                {
                    PRINT_ERR("Failed InputCredEncodingType");
                    goto error;
                }

                if (OIC_ENCODING_DER != cred->publicData.encoding &&
                    OIC_ENCODING_PEM != cred->publicData.encoding)
                {
                    PRINT_ERR("Unsupported encoding type for private key");
                    goto error;
                }

                //Read certificate data from file
                if (0 != ReadDataFromFile("\tInput the certificate's path : ", &publicKey, &publicKeySize))
                {
                    PRINT_ERR("Failed ReadDataFromFile");
                    goto error;
                }

                if (cred->optionalData.encoding == OIC_ENCODING_PEM)
                {
                    cred->publicData.data = OICCalloc(1, publicKeySize + 1);
                    if (NULL == cred->publicData.data)
                    {
                        PRINT_ERR("InputCredentialData : Failed to allocate memory.");
                        goto error;
                    }
                    cred->publicData.len = publicKeySize + 1;
                }
                else
                {
                    cred->publicData.data = OICCalloc(1, publicKeySize);
                    if (NULL == cred->publicData.data)
                    {
                        PRINT_ERR("InputCredentialData : Failed to allocate memory.");
                        goto error;
                    }
                    cred->publicData.len = publicKeySize;
                }
                memcpy(cred->publicData.data, publicKey, publicKeySize);
            }
            else
            {
                // TODO: T.B.D : Data type should be selected by user.
                PRINT_ERR("Not supported yet.");
                goto error;
            }
            break;
        case PIN_PASSWORD:
            {
                char pinPass[OXM_RANDOM_PIN_MAX_SIZE + 1] = {0};

                PRINT_NORMAL("\tSubject UUID (e.g. 61646D69-6E44-6576-6963-655575696430) : ");
                if (0 != InputUuid(&cred->subject))
                {
                    PRINT_ERR("Failed InputUuid");
                    goto error;
                }

                PRINT_NORMAL("\tInput the PIN or Password : ");
                for (int ret = 0; 1 != ret; )
                {
                    ret = scanf("%32s", pinPass);
                    for ( ; 0x20 <= getchar(); ); // for removing overflow garbages
                    // '0x20<=code' is character region
                }
                cred->privateData.data = (uint8_t *)OICStrdup(pinPass);
                if (NULL == cred->privateData.data)
                {
                    PRINT_ERR("Failed OICStrdup");
                    goto error;
                }
                cred->privateData.len = strlen((char *)cred->privateData.data);
                cred->privateData.encoding = OIC_ENCODING_RAW;
                break;
            }
        case ASYMMETRIC_ENCRYPTION_KEY:
            // TODO: T.B.D
            PRINT_INFO("Unsupported yet.");
            goto error;
            break;
        default:
            PRINT_ERR("Invalid credential type");
            goto error;
    }

    OICFree(certChain);
    OICFree(privateKey);
    OICFree(publicKey);
    return 0;

error:
    OICFree(certChain);
    OICFree(privateKey);
    OICFree(publicKey);
    memset(cred, 0x00, sizeof(OicSecCred_t));
    return -1;
}

// TODO: Update to use credresource.c
static int ParseCertChain(mbedtls_x509_crt *crt, unsigned char *buf, size_t bufLen)
{
    if (NULL == crt || NULL == buf || 0 == bufLen)
    {
        PRINT_ERR("ParseCertChain : Invalid param");
        return -1;
    }

    /* byte encoded ASCII string '-----BEGIN CERTIFICATE-----' */
    char pemCertHeader[] =
    {
        0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x42, 0x45, 0x47, 0x49, 0x4e, 0x20, 0x43, 0x45, 0x52,
        0x54, 0x49, 0x46, 0x49, 0x43, 0x41, 0x54, 0x45, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d
    };
    // byte encoded ASCII string '-----END CERTIFICATE-----' */
    char pemCertFooter[] =
    {
        0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x45, 0x4e, 0x44, 0x20, 0x43, 0x45, 0x52, 0x54, 0x49,
        0x46, 0x49, 0x43, 0x41, 0x54, 0x45, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d
    };
    size_t pemCertHeaderLen = sizeof(pemCertHeader);
    size_t pemCertFooterLen = sizeof(pemCertFooter);

    size_t len = 0;
    unsigned char *tmp = NULL;
    int count = 0;
    int ret = 0;
    size_t pos = 0;

    while (pos < bufLen)
    {
        if (buf[pos] == 0x30 && buf[pos + 1] == 0x82)
        {
            tmp = (unsigned char *)buf + pos + 1;
            ret = mbedtls_asn1_get_len(&tmp, buf + bufLen, &len);
            if (0 != ret)
            {
                PRINT_ERR("mbedtls_asn1_get_len failed: 0x%04x", ret);
                return -1;
            }

            if (pos + len < bufLen)
            {
                ret = mbedtls_x509_crt_parse_der(crt, buf + pos, len + 4);
                if (0 == ret)
                {
                    count++;
                }
                else
                {
                    PRINT_ERR("mbedtls_x509_crt_parse_der failed: 0x%04x", ret);
                }
            }
            pos += len + 4;
        }
        else if ((buf + pos + pemCertHeaderLen < buf + bufLen) &&
                 0 == memcmp(buf + pos, pemCertHeader, pemCertHeaderLen))
        {
            void *endPos = NULL;
            endPos = memmem(&(buf[pos]), bufLen - pos, pemCertFooter, pemCertFooterLen);
            if (NULL == endPos)
            {
                PRINT_ERR("end of PEM certificate not found.");
                return -1;
            }

            len = (char *)endPos - ((char *)buf + pos) + pemCertFooterLen;
            if (pos + len + 1 <= bufLen)
            {
                char con = buf[pos + len];
                buf[pos + len] = 0x00;
                ret = mbedtls_x509_crt_parse(crt, buf + pos, len + 1);
                if (0 == ret)
                {
                    count++;
                }
                else
                {
                    PRINT_ERR("mbedtls_x509_crt_parse failed: 0x%04x", ret);
                }
                buf[pos + len] = con;
            }
            else
            {
                unsigned char *lastCert = (unsigned char *)OICMalloc((len + 1) * sizeof(unsigned char));
                if (NULL == lastCert)
                {
                    PRINT_ERR("Failed to allocate memory for certificate");
                    return -1;
                }
                memcpy(lastCert, buf + pos, len);
                lastCert[len] = 0x00;
                ret = mbedtls_x509_crt_parse(crt, lastCert, len + 1);
                if (0 == ret)
                {
                    count++;
                }
                else
                {
                    PRINT_ERR("mbedtls_x509_crt_parse failed: 0x%04x", ret);
                }
                OICFree(lastCert);
            }
            pos += len;
        }
        else
        {
            pos++;
        }
    }

    return 0;
}

// TODO: Update to use credresource.c
static void ParseDerCaCert(ByteArray_t *crt, const char *usage, uint16_t credId)
{
    if (NULL == crt || NULL == usage)
    {
        PRINT_ERR("ParseDerCaCert : Invalid param");
        return;
    }
    crt->len = 0;
    OicSecCred_t *temp = NULL;

    LL_FOREACH(((OicSecCred_t *)GetCredList()), temp)
    {
        if (SIGNED_ASYMMETRIC_KEY == temp->credType &&
            0 == strcmp(temp->credUsage, usage) &&
            temp->credId == credId)
        {
            if (OIC_ENCODING_BASE64 == temp->optionalData.encoding)
            {
                size_t bufSize = B64DECODE_OUT_SAFESIZE((temp->optionalData.len + 1));
                uint8_t *buf = OICCalloc(1, bufSize);
                if (NULL == buf)
                {
                    PRINT_ERR("ParseDerCaCert : Failed to allocate memory");
                    return;
                }
                size_t outSize;
                if (B64_OK != b64Decode((char *)(temp->optionalData.data), temp->optionalData.len, buf, bufSize,
                                        &outSize))
                {
                    OICFree(buf);
                    PRINT_ERR("ParseDerCaCert : Failed to decode base64 data");
                    return;
                }
                crt->data = OICRealloc(crt->data, crt->len + outSize);
                memcpy(crt->data + crt->len, buf, outSize);
                crt->len += outSize;
                OICFree(buf);
            }
            else
            {
                crt->data = OICRealloc(crt->data, crt->len + temp->optionalData.len);
                memcpy(crt->data + crt->len, temp->optionalData.data, temp->optionalData.len);
                crt->len += temp->optionalData.len;
            }
        }
    }
    if (0 == crt->len)
    {
        PRINT_INFO("ParseDerCaCert : %s not found", usage);
    }
    return;
}

// TODO: Update to use credresource.c
static void ParseDerOwnCert(ByteArray_t *crt, const char *usage, uint16_t credId)
{
    OIC_LOG_V(DEBUG, TAG, "In %s", __func__);
    if (NULL == crt || NULL == usage)
    {
        OIC_LOG_V(DEBUG, TAG, "Out %s", __func__);
        return;
    }
    crt->len = 0;
    OicSecCred_t *temp = NULL;
    LL_FOREACH(((OicSecCred_t *)GetCredList()), temp)
    {
        if (SIGNED_ASYMMETRIC_KEY == temp->credType &&
            0 == strcmp(temp->credUsage, usage) &&
            temp->credId == credId)
        {
            crt->data = OICRealloc(crt->data, crt->len + temp->publicData.len);
            memcpy(crt->data + crt->len, temp->publicData.data, temp->publicData.len);
            crt->len += temp->publicData.len;
            OIC_LOG_V(DEBUG, TAG, "%s found", usage);
        }
    }
    if (0 == crt->len)
    {
        OIC_LOG_V(WARNING, TAG, "%s not found", usage);
    }
    OIC_LOG_V(DEBUG, TAG, "Out %s", __func__);
    return;
}

inline static void ParseDerKey(ByteArray_t *key, const char *usage, uint16_t credId)
{
    OIC_LOG_V(DEBUG, TAG, "In %s", __func__);
    if (NULL == key || NULL == usage)
    {
        OIC_LOG_V(DEBUG, TAG, "Out %s", __func__);
        return;
    }

    OicSecCred_t *temp = NULL;
    key->len = 0;
    LL_FOREACH(((OicSecCred_t *)GetCredList()), temp)
    {
        if (SIGNED_ASYMMETRIC_KEY == temp->credType &&
            0 == strcmp(temp->credUsage, usage) &&
            temp->credId == credId)
        {
            key->data = OICRealloc(key->data, key->len + temp->privateData.len);
            memcpy(key->data + key->len, temp->privateData.data, temp->privateData.len);
            key->len += temp->privateData.len;
            OIC_LOG_V(DEBUG, TAG, "Key for %s found", usage);
        }
    }
    if (0 == key->len)
    {
        OIC_LOG_V(WARNING, TAG, "Key for %s not found", usage);
    }
    OIC_LOG_V(DEBUG, TAG, "Out %s", __func__);
}


static void HandleCredOperation(SubOperationType_t cmd)
{
    uint16_t credId = 0;
    OCStackResult credResult = OC_STACK_ERROR;

    if (SVR_EDIT_IDX_SIZE <= cmd)
    {
        PRINT_ERR("Invalid menu for credential");
        return;
    }
    if (g_allowedEditMenu[cmd])
    {
        switch (cmd)
        {
            case SVR_PRINT:
                PrintCredList(GetCredList());
                break;
            case SVR_ADD:
                {
                    OicSecCred_t *cred = (OicSecCred_t *)OICCalloc(1, sizeof(OicSecCred_t));
                    if (NULL == cred)
                    {
                        PRINT_ERR("Failed to allocate memory");
                        return;
                    }

                    if (0 != InputCredentialData(cred))
                    {
                        PRINT_ERR("Failed to InputCredentialData");
                        DeleteCredList(cred);
                        return;
                    }

                    credResult = AddCredential(cred);
                    if ( OC_STACK_OK != credResult)
                    {
                        PRINT_ERR("AddCredential error : %d" , credResult);
                        DeleteCredList(cred);
                        return;
                    }

                    break;
                }
            case SVR_REMOVE:
                PrintCredList(GetCredList());
                credId = (uint16_t)InputNumber("\tPlease input the credential ID : ");

                credResult = RemoveCredentialByCredId(credId);
                if ( OC_STACK_RESOURCE_DELETED != credResult)
                {
                    PRINT_ERR("RemoveCredentialByCredId error : %d" , credResult);
                    return;
                }

                break;
            case SVR_MODIFY:
                PRINT_INFO("Unsupported yet.");
                // TODO: T.B.D
                break;
            case BACK:
                PRINT_INFO("Back to the previous menu.");
                break;
            default:
                PRINT_ERR("Invalid menu for credential");
                break;
        }
    }
    else
    {
        PRINT_ERR("Invalid menu for credential");
    }
}

static void HandleAclOperation(const SubOperationType_t cmd)
{
    OCStackResult aclResult = OC_STACK_ERROR;
    size_t aclIdx = 0;
    uint8_t *aclPayload = NULL;
    size_t aclPayloadSize = 0;

    if (SVR_EDIT_IDX_SIZE <= cmd)
    {
        PRINT_ERR("Invalid menu for ACL");
        return;
    }
    if (g_allowedEditMenu[cmd])
    {
        switch (cmd)
        {
            case SVR_PRINT:
                {
                    PrintAcl(g_aclResource);
                    break;
                }
            case SVR_ADD:
                {
                    OicSecAce_t *ace = (OicSecAce_t *)OICCalloc(1, sizeof(OicSecAce_t));
                    if (NULL == ace)
                    {
                        PRINT_ERR("Failed to allocate memory");
                        return;
                    }

                    //Input ACE
                    if (0 != InputAceData(ace))
                    {
                        PRINT_ERR("Failed to input ACE");
                        FreeACE(ace);
                        return;
                    }

                    //Add ACE
                    LL_APPEND(g_aclResource->aces, ace);

                    aclResult = AclToCBORPayload(g_aclResource, OIC_SEC_ACL_V2, &aclPayload, &aclPayloadSize);
                    if (OC_STACK_OK != aclResult)
                    {
                        PRINT_ERR("AclToCBORPayload : %d" , aclResult);
                        return;
                    }

                    aclResult = UpdateSecureResourceInPS(OIC_JSON_ACL_NAME, aclPayload, aclPayloadSize);
                    if (OC_STACK_OK != aclResult)
                    {
                        PRINT_ERR("UpdateSecureResourceInPS : %d" , aclResult);
                        return;
                    }

                    break;
                }
            case SVR_REMOVE:
                {
                    OicSecAce_t *ace = NULL;
                    OicSecAce_t *tempAce = NULL;
                    uint16_t curIdx = 0;

                    size_t numOfAce = PrintAcl(g_aclResource);
                    aclIdx = (uint16_t)InputNumber("\tPlease input the number of ACE : ");

                    if (0 == aclIdx || aclIdx > numOfAce)
                    {
                        PRINT_ERR("Wrong number of ACE.");
                        return;
                    }

                    //Revmoe the ACE
                    LL_FOREACH_SAFE(g_aclResource->aces, ace, tempAce)
                    {
                        if (ace)
                        {
                            //If found target ACE, delete it.
                            if (aclIdx == ++curIdx)
                            {
                                LL_DELETE(g_aclResource->aces, ace);
                                FreeACE(ace);

                                aclResult = AclToCBORPayload(g_aclResource, OIC_SEC_ACL_V2, &aclPayload, &aclPayloadSize);
                                if (OC_STACK_OK != aclResult)
                                {
                                    PRINT_ERR("AclToCBORPayload : %d" , aclResult);
                                    return;
                                }

                                aclResult = UpdateSecureResourceInPS(OIC_JSON_ACL_NAME, aclPayload, aclPayloadSize);
                                if (OC_STACK_OK != aclResult)
                                {
                                    PRINT_ERR("UpdateSecureResourceInPS : %d", aclResult);
                                    return;
                                }
                            }
                        }
                    }
                    break;
                }
            case SVR_MODIFY:
                PRINT_INFO("Not supported yet.");
                // TODO: T.B.D
                break;
            case BACK:
                PRINT_INFO("Back to the previous menu.");
                break;
            default:
                PRINT_ERR("Invalid menu for ACL");
                break;
        }
    }
    else
    {
        PRINT_ERR("Invalid menu for ACL");
    }

}

inline static void HandleDoxmOperation(const SubOperationType_t cmd)
{
    (void)cmd;
    //T.B.D
}

inline static void HandlePstatOperation(const SubOperationType_t cmd)
{
    (void)cmd;
    //T.B.D
}
