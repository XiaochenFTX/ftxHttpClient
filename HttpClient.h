//
// Created by 王晓辰 on 16/3/18.
//

#ifndef FTXHTTPCLIENT_HTTPCLIENT_H
#define FTXHTTPCLIENT_HTTPCLIENT_H

#include <string>
#include <map>


namespace ftx {

//
class HttpParams
{
public:
    HttpParams(){}
    HttpParams(const std::map<std::string, std::string>& params);

    void Add(const std::string& key, const std::string& val);
    std::string ToString();

private:
    std::map<std::string, std::string> _params;
};

class HttpClient {
public:
    static void StartUp(long max_connects = 20);
    static void ShutDown();
    static void Loop();

    static void PushDownload(const std::string& url, const std::string& filepath
            , std::function<void(bool, std::string)> callback = nullptr /* void (bool isSucceed, string filepath) */
            , size_t block_size = 20 /* MB */
            , bool need_resume = true);

    static double DownloadSpeed(const std::string& filepath);
    static double DownloadSize(const std::string& filepath);
    static std::tuple<double, double> DownloadSpeedAndSize(const std::string& filepath);
    static double DownloadAllSpeed();
    static void ClearDownload(const std::string& filepath);

    static std::string FilePath2TmpPath(const std::string& filepath);
    static std::string FileLogFullPath(const std::string& filepath);

    /* void (long responseCode, string result) */
    static void RequestGet(const std::string& url, std::function<void(long code, std::string data)> callback = nullptr);
    static void RequestPost(const std::string& url, const std::string& params_str = ""
            , std::function<void(long code, std::string data)> callback = nullptr);

private:
    static void curlPerformLoop();

private:
    static bool httpThreadAlive;
    static long maxConnects;
    static long maxDownloadConnects;
};

}
#endif //FTXHTTPCLIENT_HTTPCLIENT_H
