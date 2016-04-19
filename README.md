# ftxHttpClient
Http Client via libcurl

# example

    ftx::HttpClient::StartUp();
    ftx::HttpClient::PushDownload("http://.......", "..../test.zip"
    , [](bool succeed, std::string filepath){
                printf("download call back: %s\n", filepath.c_str());
            });
    
    ftx::HttpClient::RequestGet("http://......?a=xxx&b=YYY", [](long code, std::string result){
        
    });
    
    ftx::HttpParams params;
    params.Add("x", "aaaaa");
    params.Add("y", "BBBBB");
    
    ftx::HttpClient::RequestPost("http://......", params.ToString(), [](long code, std::string result){
        
    });
    
    ftx::HttpOption opt;
    opt.userAgent = "ftxHttpClient";
    opt.certFile = "......./ssl-cert-snakeoil.pem";
    opt.useSSL = true;
    opt.verifyPeer = true;
    opt.verifyHost = false;
    opt.useHttp2 = true;
    opt.verbose = true;

    ftx::HttpClient::RequestGetEx("https://......?a=xxx&b=YYY", opt, [](long code, std::string result){
        
    });
    
    while(true)
    {
        sleep(1);
        double speed = ftx::HttpClient::DownloadAllSpeed();
        ftx::HttpClient::Loop();
        printf("all speed: %lf\n", speed);
    }
    
    ftx::HttpClient::ShutDown();