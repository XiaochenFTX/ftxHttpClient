//
// Created by 王晓辰 on 16/3/18.
//

#include "HttpClient.h"

#include <curl/curl.h>

#include <sstream>
#include <fstream>
#include <thread>
#include <vector>
#include <deque>


const char* TEMP_FILE_SUFFIX = ".ftxtmp";
const char* FILE_LOG_SUFFIX = ".ftxlog";
const long CONNECTS_FOR_DOWNLOAD = 10;

ftx::HttpParams::HttpParams(const std::map<std::string, std::string> &params)
: _params(params)
{

}

void ftx::HttpParams::Add(const std::string &key, const std::string &val)
{
    _params[key] = val;
}

std::string ftx::HttpParams::ToString()
{
    std::stringstream ss;

    for (auto iter = _params.cbegin(); iter != _params.cend(); ++iter)
    {
        if (iter != _params.cbegin())
        {
            ss << "&";
        }

        ss << iter->first;
        ss << "=";
        ss << iter->second;
    }

    return ss.str();
}

// ========================================================

namespace ftx {

    class HttpTaskManager
    {
        void PushToBackgroundThread(std::function<void()> task)
        {
            backgroundMtx.lock();
            backgroundTasks.push_back(task);
            backgroundMtx.unlock();
        }

        void BackgroundLoop()
        {
            PerformAll();
        }

        void PerformAll()
        {
            backgroundMtx.lock();
            if (backgroundTasks.empty())
            {
                backgroundMtx.unlock();
                return ;
            }

            std::vector<std::function<void()>> temp(backgroundTasks);
            backgroundTasks.clear();
            backgroundMtx.unlock();

            for (auto func: temp)
            {
                func();
            }
        }

        void PushToForeground(std::function<void()> tasks)
        {
            foregroundMtx.lock();
            foregroundTasks.push_back(tasks);
            foregroundMtx.unlock();
        }

        void ForegroundLoop()
        {
            foregroundMtx.lock();
            if (foregroundTasks.empty())
            {
                foregroundMtx.unlock();
                return ;
            }

            std::vector<std::function<void()>> temp(foregroundTasks);
            foregroundTasks.clear();
            foregroundMtx.unlock();

            for (auto func: temp)
            {
                func();
            }
        }

        std::mutex backgroundMtx;
        std::mutex foregroundMtx;
        std::vector<std::function<void()>> backgroundTasks;
        std::vector<std::function<void()>> foregroundTasks;

        friend class HttpClient;
    };

    HttpTaskManager httpTaskManager;

    // ==============================================

    struct BlockList
    {
        std::vector<std::tuple<long, long>> all;
    };

    struct DownloadBlock
    {
        CURL* handle;
        FILE* fd;
        long start;
        long end;
        bool resume;
        size_t index;
        std::string filepath;
    };

    struct RequestStream
    {
        CURL* handle;
        size_t id;
        std::stringstream stream;
    };

    enum class RequestType
    {
        HttpRequest,
        HttpDownload
    };

    struct RequestTypeOption
    {
        RequestType type;
        void* data;
    };

    std::vector<DownloadBlock*> blockPool;
    static DownloadBlock* takeDownloadBlock(CURL* handle, long start, long end, bool resume, size_t index, const std::string& filepath)
    {
        DownloadBlock* block;
        if (!blockPool.empty())
        {
            block = blockPool.back();
            blockPool.pop_back();
        }
        else
        {
            block = new DownloadBlock();
        }

        std::string tmpfilepath = HttpClient::FilePath2TmpPath(filepath);
        FILE* fd = fopen(tmpfilepath.c_str(), "wb");
        fseek(fd, start, SEEK_SET);

        block->handle = handle;
        block->fd = fd;
        block->start = start;
        block->end = end;
        block->resume = resume;
        block->index = index;
        block->filepath = filepath;

        return block;
    }

    static void putbackDownloadBlock(DownloadBlock* block)
    {
        fflush(block->fd);
        fclose(block->fd);

        blockPool.push_back(block);
    }

    static void clearDownloadBlockPool()
    {
        for(auto block: blockPool)
        {
            delete block;
        }

        blockPool.clear();
    }

    std::vector<RequestStream*> reqStreamPool;
    static RequestStream* takeRequestStream(CURL* handle, size_t id)
    {
        RequestStream* stream;
        if (!reqStreamPool.empty())
        {
            stream = reqStreamPool.back();
            reqStreamPool.pop_back();
        }
        else
        {
            stream = new RequestStream();
        }

        stream->handle = handle;
        stream->id = id;

        return stream;
    }

    static void putbackRequestStream(RequestStream* stream)
    {
        stream->stream.clear();
        reqStreamPool.push_back(stream);
    }

    static void clearRequestStream()
    {
        for(auto stream: reqStreamPool)
        {
            delete stream;
        }

        reqStreamPool.clear();
    }

    std::vector<RequestTypeOption*> requestOptionPool;
    static RequestTypeOption* takeRequestOption(RequestType type, void* data)
    {
        RequestTypeOption* opt;
        if (!requestOptionPool.empty())
        {
            opt = requestOptionPool.back();
            requestOptionPool.pop_back();
        }
        else
        {
            opt = new RequestTypeOption();
        }

        opt->type = type;
        opt->data = data;

        return opt;
    }

    static void putbackRequestOption(RequestTypeOption* opt)
    {
        if (opt->type == RequestType::HttpDownload)
        {
            putbackDownloadBlock((DownloadBlock*)opt->data);
        }
        else if (opt->type == RequestType::HttpRequest)
        {
            putbackRequestStream((RequestStream*) opt->data);
        }

        requestOptionPool.push_back(opt);
    }

    static void clearRequestOptionPool()
    {
        for(auto opt: requestOptionPool)
        {
            delete opt;
        }

        requestOptionPool.clear();
    }

    class FileTool
    {
    public:
        static BlockList LoadBlocks(const std::string& logfile)
        {
            BlockList blockList;

            std::ifstream ifs;
            ifs.open(logfile, std::ios::in | std::ios::binary);

            if (!ifs.is_open())
            {
                return blockList;
            }

            size_t size = 0;
            ifs >> size;

            for (auto i = 0; i < size; ++i)
            {
                long begin = 0;
                long end = 0;
                ifs >> begin;
                ifs >> end;

                if (begin < end)
                {
                    blockList.all.push_back(std::make_tuple(begin, end));
                }
            }

            ifs.close();

            return blockList;
        }

        static void WriteBlocks(const std::string& logfile, const BlockList& blockList)
        {
            std::ofstream ofs;
            ofs.open(logfile, std::ios::out | std::ios::binary);

            if (!ofs.is_open())
            {
                return ;
            }

            size_t size = blockList.all.size();
            ofs << size;
            for (auto block: blockList.all)
            {
                long begin = 0;
                long end = 0;
                std::tie(begin, end) = block;

                ofs << begin;
                ofs << end;
            }

            ofs.flush();
            ofs.close();
        }

        static void UpdateBlocks(const std::string& logfile, size_t index, long start)
        {
            std::ofstream ofs;
            ofs.open(logfile, std::ios::out | std::ios::binary);

            size_t pos = sizeof(size_t);
            pos += index * sizeof(long) * 2;

            ofs.seekp(pos);

            ofs << start;

            ofs.flush();
            ofs.close();
        }

        static void DowdloadFinish(const std::string& filepath)
        {
            std::string tmp_file_path = HttpClient::FilePath2TmpPath(filepath);
            std::rename(tmp_file_path.c_str(), filepath.c_str());
            std::string log_file_path = HttpClient::FileLogFullPath(filepath);
            std::remove(log_file_path.c_str());
        }

        static void ClearTempAndLogFiles(const std::string& filepath)
        {
            std::string tmp_file_path = HttpClient::FilePath2TmpPath(filepath);
            std::remove(tmp_file_path.c_str());
            std::string log_file_path = HttpClient::FileLogFullPath(filepath);
            std::remove(log_file_path.c_str());
        }

        friend class HttpClient;
    };

    class DownloadDashboard
    {
    public:
        void UpdateInfo(const std::string& filepath, size_t index, double speed, double size)
        {
            downloadInfo[filepath][index] = std::make_tuple(speed, size);
        }

        void Remove(const std::string& filepath)
        {
            downloadInfo.erase(filepath);
        }

        std::tuple<double, double> SpeedAndSize(const std::string& filepath)
        {
            auto curr_file = downloadInfo[filepath];
            double speed = 0;
            double size = 0;
            for (auto info: curr_file)
            {
                speed += std::get<0>(info.second);
                size += std::get<1>(info.second);
            }

            return std::make_tuple(speed, size);
        };

        double Speed(const std::string& filepath)
        {
            auto curr_file = downloadInfo[filepath];
            double speed = 0;
            for (auto info: curr_file)
            {
                speed += std::get<0>(info.second);
            }

            return speed;
        }

        double Size(const std::string& filepath)
        {
            auto curr_file = downloadInfo[filepath];
            double size = 0;
            for (auto info: curr_file)
            {
                size += std::get<1>(info.second);
            }

            return size;
        }

        double AllSpeed()
        {
            double speed;
            for (auto fileInfo: downloadInfo)
            {
                for (auto blockInfo: fileInfo.second)
                {
                    speed += std::get<0>(blockInfo.second);
                }
            }

            return speed;
        }

    private:
        std::map<std::string, std::map<long, std::tuple<double, double>>> downloadInfo;
    };

    DownloadDashboard downloadDashboard;

    enum class DownloadResult
    {
        None,
        Succeed,
        Failed,
    };
    std::map<std::string, std::map<long, DownloadResult>> downloadResultTable;

    // ==============================================

    CURLM* curlm = nullptr;
    std::deque<CURL*> waitRequestHandles;
    std::deque<CURL*> waitDownloadHandles;

    static CURL* setCurlOptEx(CURL** handle, const HttpOption& opt)
    {
        curl_easy_setopt(*handle, CURLOPT_VERBOSE, opt.verbose);

        if (!opt.userAgent.empty())
        {
            curl_easy_setopt(*handle, CURLOPT_USERAGENT, opt.userAgent.c_str());
        }

        if (opt.useSSL)
        {
            curl_easy_setopt(*handle, CURLOPT_SSL_VERIFYPEER, opt.verifyPeer);
            curl_easy_setopt(*handle, CURLOPT_SSL_VERIFYHOST, opt.verifyHost);

            if (!opt.certFile.empty())
            {
                curl_easy_setopt(*handle, CURLOPT_CAINFO, opt.certFile.c_str());
            }

            curl_easy_setopt(*handle, CURLOPT_PIPEWAIT, opt.useHttp2);
        }

        return *handle;
    }

    static CURL* requestHttpHeader(const char* url, const HttpOption& opt)
    {
        CURL *handle = curl_easy_init ();
        curl_easy_setopt (handle, CURLOPT_URL, url);
        curl_easy_setopt (handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt (handle, CURLOPT_AUTOREFERER, 1L);
        curl_easy_setopt (handle, CURLOPT_HEADER, 0L);
        curl_easy_setopt (handle, CURLOPT_NOBODY, 1L);
        curl_easy_setopt (handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt (handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);

        setCurlOptEx(&handle, opt);

        if (curl_easy_perform (handle) != CURLE_OK)
        {
            return nullptr;
        }

        return handle;
    }

    static long getDownloadFileLength(const char* url, const HttpOption& opt)
    {
        double file_length = -1;
        CURL* curl = requestHttpHeader(url, opt);

        if (curl != nullptr)
        {
            curl_easy_getinfo (curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &file_length);
        }

        return (long)file_length;
    }

    static size_t downloadWriteData(void *ptr, size_t size, size_t nmemb, void *stream)
    {
        DownloadBlock* block = (DownloadBlock*)stream;

        size_t written = fwrite(ptr, size, nmemb, block->fd);
        block->start += size * nmemb;

        double speed;
        double already;
        curl_easy_getinfo(block->handle, CURLINFO_SPEED_DOWNLOAD, &speed);
        curl_easy_getinfo(block->handle, CURLINFO_SIZE_DOWNLOAD, &already);

        downloadDashboard.UpdateInfo(block->filepath, block->index, speed, already);

        if (block->resume)
        {
            FileTool::UpdateBlocks(HttpClient::FileLogFullPath(block->filepath), block->index, block->start);
        }

        return written;
    }

    static void pushDownload(const std::string& url, const BlockList& blockList, const std::string& filepath
            , bool resume, const HttpOption& opt)
    {
        for (size_t i = 0; i < blockList.all.size(); ++i)
        {
            CURL* curl = curl_easy_init();
            long start;
            long end;
            char range[64];
            auto info = blockList.all[i];

            std::tie(start, end) = info;
            sprintf(range, "%ld-%ld", start, end);

            DownloadBlock* block = takeDownloadBlock(curl, start, end, resume, i, filepath);
            RequestTypeOption* reqtype = takeRequestOption(RequestType::HttpDownload, block);

            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, downloadWriteData);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, block);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_PRIVATE, reqtype);
            curl_easy_setopt(curl, CURLOPT_RANGE, range);
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);

            setCurlOptEx(&curl, opt);

            waitDownloadHandles.push_back(curl);
            downloadResultTable[filepath][i] = DownloadResult::None;
        }
    }

    std::map<std::string, std::function<void(bool, std::string)>> downloadCallbackMap;

    // =========================================
    static size_t httpIndex = 0;
    static size_t newIndex()
    {
        return httpIndex++;
    }

    static size_t requestWriteData(void *ptr, size_t size, size_t nmemb, void *stream)
    {
        RequestStream* response = (RequestStream*)stream;
        size_t length = size * nmemb;

        response->stream.write((const char*)ptr, length);

        return length;
    }

    std::map<size_t, std::string> postParamMap;

    static void pushHttpRequest(const std::string& url, size_t index, bool post, const std::string& params_str
            , const HttpOption& opt)
    {
        CURL* curl = curl_easy_init();

        RequestStream* stream = takeRequestStream(curl, index);
        RequestTypeOption* reqtype = takeRequestOption(RequestType::HttpRequest, stream);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, requestWriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, stream);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_PRIVATE, reqtype);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);

        setCurlOptEx(&curl, opt);

        if (post)
        {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (!params_str.empty())
            {
                postParamMap[index] = params_str;
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postParamMap[index].c_str());
            }
        }

        waitRequestHandles.push_back(curl);
    }

    std::map<size_t, std::function<void(long, std::string)>> httpResponseMap;

    // =========================================
    static HttpOption defaultHttpOption(const std::string& url)
    {
        HttpOption opt;

        opt.useSSL = url.substr(0, 5) == "https";
        opt.verifyHost = opt.useSSL;
        opt.verifyPeer = opt.useSSL;
        opt.useHttp2 = opt.useSSL;
        opt.userAgent = "ftxHttpClient";

        return opt;
    }
}

// ==============================================

void ftx::HttpClient::StartUp(long max_connects)
{
    httpThreadAlive = true;
    maxConnects = max_connects;
    maxDownloadConnects = CONNECTS_FOR_DOWNLOAD;

    httpTaskManager.PushToBackgroundThread([]() {
        curl_global_init(CURL_GLOBAL_ALL);
        curlm = curl_multi_init();

        curl_multi_setopt(curlm, CURLMOPT_MAXCONNECTS, maxConnects);
        curl_multi_setopt(curlm, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    });

    std::thread http_thread([&](){
        while(httpThreadAlive)
        {
            httpTaskManager.BackgroundLoop();
            curlPerformLoop();
        }

        if (!httpThreadAlive)
        {
            curl_multi_cleanup(curlm);
            curl_global_cleanup();
        }
    });

    http_thread.detach();
}

void ftx::HttpClient::ShutDown()
{
    httpThreadAlive = false;
    clearDownloadBlockPool();
    clearRequestOptionPool();
}

void ftx::HttpClient::Loop()
{
    httpTaskManager.ForegroundLoop();
}

void ftx::HttpClient::PushDownload(const std::string& url, const std::string& filepath
        , std::function<void(bool, std::string)> callback, size_t block_size, bool need_resume)
{
    HttpOption opt = defaultHttpOption(url);
    PushDownloadEx(url, filepath, opt, callback, block_size, need_resume);
}

void ftx::HttpClient::PushDownloadEx(const std::string &url, const std::string &filepath, const HttpOption &opt
        , std::function<void(bool, std::string)> callback, size_t block_size, bool need_resume)
{
    httpTaskManager.PushToBackgroundThread([=]() {
        BlockList blockList;

        auto recomputaion = [url, block_size, opt]() -> BlockList
        {
            BlockList blockList;
            size_t block_size_byte = block_size * 1024 * 1024;

            long filesize = getDownloadFileLength(url.c_str(), opt);
            long begin = 0;
            while (begin < filesize)
            {
                long end = std::min(filesize, (long)(begin + block_size_byte));
                blockList.all.push_back(std::make_tuple(begin, end));
                begin += block_size_byte;
            }

            return blockList;
        };

        if (need_resume)
        {
            std::string logfile = FileLogFullPath(filepath);
            blockList = FileTool::LoadBlocks(logfile);
            if (blockList.all.empty())
            {
                blockList = recomputaion();
                FileTool::WriteBlocks(logfile, blockList);
            }
        }
        else
        {
            blockList = recomputaion();
        }

        //
        pushDownload(url, blockList, filepath, need_resume, opt);
    });

    downloadCallbackMap[filepath] = callback;
}

double ftx::HttpClient::DownloadSpeed(const std::string &filepath)
{
    return downloadDashboard.Speed(filepath);
}

double ftx::HttpClient::DownloadSize(const std::string &filepath)
{
    return downloadDashboard.Size(filepath);
}

std::tuple<double, double> ftx::HttpClient::DownloadSpeedAndSize(const std::string &filepath)
{
    return downloadDashboard.SpeedAndSize(filepath);
}

double ftx::HttpClient::DownloadAllSpeed()
{
    return downloadDashboard.AllSpeed();
}

void ftx::HttpClient::ClearDownload(const std::string &filepath)
{
    FileTool::ClearTempAndLogFiles(filepath);
}

std::string ftx::HttpClient::FilePath2TmpPath(const std::string &filepath)
{
    return filepath + TEMP_FILE_SUFFIX;
}

std::string ftx::HttpClient::FileLogFullPath(const std::string &filepath)
{
    return filepath + FILE_LOG_SUFFIX;
}

void ftx::HttpClient::curlPerformLoop()
{
    CURLMsg* msg;
    int handles, max_fd, msgs = -1;
    long wait_timeout;
    fd_set read_fd, write_fd, exec_fd;
    struct timeval T;

    curl_multi_perform(curlm, &handles);

    if(handles != 0)
    {
        FD_ZERO(&read_fd);
        FD_ZERO(&write_fd);
        FD_ZERO(&exec_fd);

        if(curl_multi_fdset(curlm, &read_fd, &write_fd, &exec_fd, &max_fd))
        {
            fprintf(stderr, "E: curl_multi_fdset\n");
            return;
        }

        if(curl_multi_timeout(curlm, &wait_timeout))
        {
            fprintf(stderr, "E: curl_multi_timeout\n");
            return ;
        }
        if(wait_timeout == -1)
            wait_timeout = 100;

        if(max_fd == -1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_timeout));
        }
        else
        {
            T.tv_sec = wait_timeout / 1000;
            T.tv_usec = (wait_timeout % 1000) * 1000;

            if(0 > select(max_fd+1, &read_fd, &write_fd, &exec_fd, &T))
            {
                fprintf(stderr, "E: select(%i,,,,%li): %i: %s\n",
                        max_fd+1, wait_timeout, errno, strerror(errno));
                return;
            }
        }
    }

    while((msg = curl_multi_info_read(curlm, &msgs)))
    {
        CURL *e = msg->easy_handle;
        RequestTypeOption* opt;
        long responseCode;
        curl_easy_getinfo(e, CURLINFO_PRIVATE, &opt);
        curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &responseCode);

        RequestType type = opt->type;

        bool success = responseCode >= 200 && responseCode < 300;

        if (type == RequestType::HttpDownload)
        {
            DownloadBlock* block = (DownloadBlock*) opt->data;
            double already;
            curl_easy_getinfo(block->handle, CURLINFO_SIZE_DOWNLOAD, &already);

            downloadDashboard.UpdateInfo(block->filepath, block->index, 0, already);

            downloadResultTable[block->filepath][block->index] = success ? DownloadResult::Succeed: DownloadResult::Failed;

            DownloadResult downloadResult = DownloadResult::Succeed;
            for(auto res: downloadResultTable[block->filepath])
            {
                if (res.second == DownloadResult::None)
                {
                    downloadResult = DownloadResult::None;
                }
                else if (res.second == DownloadResult::Failed)
                {
                    downloadResult = DownloadResult::Failed;
                }
            }

            if (downloadResult != DownloadResult::None)
            {
                if (downloadResult == DownloadResult::Succeed)
                {
                    FileTool::DowdloadFinish(block->filepath);
                }

                bool succeed = downloadResult == DownloadResult::Succeed;
                std::string filepath = block->filepath;
                httpTaskManager.PushToForeground([succeed, filepath](){
                    auto callback = downloadCallbackMap[filepath];
                    if (callback != nullptr)
                    {
                        callback(succeed, filepath);
                    }

                    downloadCallbackMap.erase(filepath);
                });

                downloadResultTable.erase(block->filepath);
                downloadDashboard.Remove(block->filepath);
            }
        }
        else if (type == RequestType::HttpRequest)
        {
            RequestStream* stream = (RequestStream*) opt->data;
            size_t id = stream->id;
            stream->stream.flush();
            std::string result = stream->stream.str();

            httpTaskManager.PushToForeground([responseCode, id, result](){
                auto callback = httpResponseMap[id];
                if (callback != nullptr)
                {
                    callback(responseCode, result);
                }

                httpResponseMap.erase(id);
            });

            postParamMap.erase(id);
        }

        putbackRequestOption(opt);

        curl_multi_remove_handle(curlm, e);
        curl_easy_cleanup(e);
        --handles;
    }

    while (handles < maxDownloadConnects && !waitDownloadHandles.empty())
    {
        CURL* curl = waitDownloadHandles.front();
        waitDownloadHandles.pop_front();
        curl_multi_add_handle(curlm, curl);
        ++handles;
    }

    size_t counter  = 0;
    while (handles < maxConnects && !waitRequestHandles.empty() && counter < maxConnects - maxDownloadConnects)
    {
        CURL* curl = waitRequestHandles.front();
        waitRequestHandles.pop_front();
        curl_multi_add_handle(curlm, curl);
        ++handles;
        ++counter;
    }
}

void ftx::HttpClient::RequestGet(const std::string &url, std::function<void(long, std::string)> callback)
{
    HttpOption opt = defaultHttpOption(url);
    RequestGetEx(url, opt, callback);
}

void ftx::HttpClient::RequestPost(const std::string &url, const std::string &params_str
        , std::function<void(long, std::string)> callback)
{
    HttpOption opt = defaultHttpOption(url);
    RequestPostEx(url, opt, params_str, callback);
}

void ftx::HttpClient::RequestGetEx(const std::string &url, const HttpOption &opt
        , std::function<void(long, std::string)> callback)
{
    size_t index = newIndex();
    httpTaskManager.PushToBackgroundThread([=](){
        pushHttpRequest(url, index, false, "", opt);
    });

    httpResponseMap[index] = callback;
}

void ftx::HttpClient::RequestPostEx(const std::string &url, const HttpOption &opt, const std::string &params_str
        , std::function<void(long, std::string)> callback)
{
    size_t index = newIndex();
    httpTaskManager.PushToBackgroundThread([=](){
        pushHttpRequest(url, index, true, params_str, opt);
    });

    httpResponseMap[index] = callback;
}

bool ftx::HttpClient::httpThreadAlive = false;
long ftx::HttpClient::maxConnects = 20;
long ftx::HttpClient::maxDownloadConnects = 10;