#include <unistd.h>
#include <glob.h>

#include "clamav_engine.h"

ClamavEngine::ClamavEngine(const char* path)
    : db_path_(path)
    , engine_(NULL)
    , dbstat_(NULL)
    , sig_num_(0)
{
    int ret;
    if ((ret = cl_init(CL_INIT_DEFAULT)) != CL_SUCCESS)
    {
        printf("Can't initialize libclamav: %s\n", cl_strerror(ret));
        err_info_ = std::string("Can't initialize libclamav");
    }

    if (!(engine_ = cl_engine_new()))
    {
        printf("Can't create new engine\n");
        err_info_ = std::string("Can't create new engine");
    }
    dbstat_ = new  struct cl_stat;
    if (cl_statinidir(db_path_.c_str(), dbstat_) != 0)
    {
        err_info_ = std::string("cl_statinidir error");
    }
    settingMaskInit();
}

ClamavEngine::~ClamavEngine()
{
    if (dbstat_ != NULL)
    {
        cl_statfree(dbstat_);
        delete dbstat_;
    }
    cl_engine_free(engine_);
}

std::string ClamavEngine::getClamavVersion()
{
    return std::string(cl_retver());
}

int ClamavEngine::reBuildEngine()
{
    struct cl_settings *settings = NULL;
    if (engine_)
    {
        settings = cl_engine_settings_copy(engine_);
        if (!settings)
        {
            printf("cl_engine_settings_copy error\n");
        }
        cl_engine_free(engine_);
    }
    engine_ = cl_engine_new();
    if (settings && engine_)
    {
        int retval = cl_engine_settings_apply(engine_, settings);
        if (retval != CL_SUCCESS)
        {
            printf("cl_engine_settings_apply error\n");
        }
        cl_engine_settings_free(settings);
    }
    return buildEngine();
}

int ClamavEngine::setSettings(ClamavSettings* settings)
{
    if (settings->max_file_size != 0)
        cl_engine_set_num(engine_, CL_ENGINE_MAX_FILESIZE, settings->max_file_size);
    if (settings->max_scan_size != 0)
        cl_engine_set_num(engine_, CL_ENGINE_MAX_SCANSIZE, settings->max_scan_size);

    getSettings();
    return 0;
}

int ClamavEngine::getSettings()
{
    long long val;
    val = cl_engine_get_num(engine_, CL_ENGINE_MAX_FILESIZE, NULL);
    printf("CL_ENGINE_MAX_FILESIZE = %lld, ", val);
    val = cl_engine_get_num(engine_, CL_ENGINE_MAX_SCANSIZE, NULL);
    printf("CL_ENGINE_MAX_SCANSIZE = %lld\n", val);
    return 0;
}


static cl_error_t pre_scan(int fd, const char *type, void *context)
{

    ClamavScanResult* result = (ClamavScanResult*)context;
    snprintf(result->type, sizeof(result->type), "%s", type);
    return CL_CLEAN;
}

int ClamavEngine::buildEngine()
{
    unsigned int sigs = 0;
    if (NULL == engine_)
    {
        err_info_ = std::string("engine_ init NULL");
        return -1;
    }

    if (access(db_path_.c_str(), F_OK) != 0)
    {
        err_info_ = std::string("database path not found");
        return -1;
    }

    glob_t buf;
    std::string cvd_pattern = db_path_ + "/*.cvd";
    glob(cvd_pattern.c_str(), GLOB_NOSORT, NULL, &buf); 
    cvd_info_list_.clear();                                                                               
    for (int i=0; i < buf.gl_pathc; i++)  
    {  
        struct cl_cvd* cvd_header_p = cl_cvdhead(buf.gl_pathv[i]);
        ClamavVDInfo cvd_info;
        cvd_info.name = buf.gl_pathv[i];
        cvd_info.info = cvd_header_p;
        cvd_info_list_.push_back(cvd_info);
    } 
    globfree(&buf);  

    int ret = 0;
    if ((ret = cl_load(db_path_.c_str(), engine_, &sigs, CL_DB_STDOPT)) != CL_SUCCESS)
    {
        cl_engine_free(engine_);
        engine_ = NULL;
        err_info_ = std::string("database load fail:");
        err_info_ += cl_strerror(ret);
        return -1;
    }

    if ((ret = cl_engine_compile(engine_)) != CL_SUCCESS)
    {
        cl_engine_free(engine_);
        engine_ = NULL;
        err_info_ = std::string("database compile fail:");
        err_info_ += cl_strerror(ret);
        return -1;
    }


    cl_engine_set_clcb_pre_scan(engine_, pre_scan);

    sig_num_ = sigs;
    err_info_ = "OK";
    return 0;
}

int ClamavEngine::scanFileFd(int fd , ClamavScanResult* result, uint32_t scan_opt)
{
    int ret = cl_scandesc_callback(fd, &result->virname, &result->size, engine_, scan_opt, result);
    if (ret == CL_VIRUS)
    {
        result->stat = kScanStatIsVIRUS;
    }
    else
    {
        if (ret == CL_CLEAN)
        {
            result->stat = kScanStatNotVIRUS;
        }
        else
        {
            printf("Error: %s\n", cl_strerror(ret));
            result->stat = kScanStatUnknow;
            return -1;
        }
    }
    return 0;
}

int ClamavEngine::ScanFmap(void* ptr, size_t len, ClamavScanResult* result, uint32_t scan_opt)
{

    cl_fmap_t* map = cl_fmap_open_memory(ptr, len);
    if (NULL == map) 
    {
        result->stat = kScanStatUnknow;
        return -1;
    }
    int ret = cl_scanmap_callback(map, &result->virname, &result->size, engine_, scan_opt, result);
    if ((ret == CL_VIRUS))
    {
        result->stat = kScanStatIsVIRUS;
    }
    else
    {
        if (ret == CL_CLEAN)
        {
            result->stat = kScanStatNotVIRUS;
        }
        else
        {
            printf("Error: %s\n", cl_strerror(ret));
            cl_fmap_close(map);
            result->stat = kScanStatUnknow;
            return -1;
        }
    }
    cl_fmap_close(map);
    return 0;
}


int ClamavEngine::scanFileFdRaw(int fd , ClamavScanResult* result)
{
    return scanFileFd(fd, result, CL_SCAN_RAW);
}

int ClamavEngine::scanFileFdStd(int fd , ClamavScanResult* result)
{
    return scanFileFd(fd, result, CL_SCAN_STDOPT);
}

int ClamavEngine::checkDatebaseChanged()
{
    if (dbstat_)
    {
        int ret = cl_statchkdir(dbstat_);
        if (1 == ret)
        {
            cl_statfree(dbstat_);
            if (cl_statinidir(db_path_.c_str(), dbstat_) != 0)
            {
                err_info_ = std::string("cl_statinidir error");
                printf("%s\n", err_info_.c_str());
            }
        }
        return ret;
    }
    return 0;
}

void ClamavEngine::settingMaskInit()
{
    setting_mask_map_["ARCHIVE"] = CL_SCAN_ARCHIVE;
    setting_mask_map_["MAIL"] = CL_SCAN_MAIL;
    setting_mask_map_["OLE2"] = CL_SCAN_OLE2;
    setting_mask_map_["PDF"] = CL_SCAN_PDF;
    setting_mask_map_["HTML"] = CL_SCAN_HTML;
    setting_mask_map_["PE"] = CL_SCAN_PE;
    setting_mask_map_["ALGORITHMIC"] = CL_SCAN_ALGORITHMIC;
    setting_mask_map_["ELF"] = CL_SCAN_ELF;
    setting_mask_map_["SWF"] = CL_SCAN_SWF;
    setting_mask_map_["XMLDOCS"] = CL_SCAN_XMLDOCS;
    setting_mask_map_["HWP3"] = CL_SCAN_HWP3;
}

uint32_t ClamavEngine::scanSettingMask(const char* setting_section)
{
    if (setting_mask_map_.find(setting_section) != setting_mask_map_.end())
    {
        return setting_mask_map_[setting_section];
    }
    return 0;
}
