#include "wvptest.h"

#include "wvp/win32/ProxySettings.h"

using wvp::platform::win32::ProxyFromSettings;

TEST_CASE(SystemProxy_OneServerForEveryScheme) {
    CHECK_EQ(ProxyFromSettings("proxy:8080", "", "http", "example.com"), std::string("proxy:8080"));
    CHECK_EQ(ProxyFromSettings("proxy:8080", "", "https", "example.com"), std::string("proxy:8080"));
    CHECK_EQ(ProxyFromSettings("", "", "http", "example.com"), std::string());
}

TEST_CASE(SystemProxy_PerSchemeServers) {
    const char* server = "http=hp:80;https=sp:443;socks=sk:1080";
    CHECK_EQ(ProxyFromSettings(server, "", "http", "a.com"), std::string("hp:80"));
    CHECK_EQ(ProxyFromSettings(server, "", "HTTPS", "a.com"), std::string("sp:443"));
    CHECK_EQ(ProxyFromSettings("http=hp:80;socks=sk:1080", "", "https", "a.com"), std::string("socks4://sk:1080"));
    CHECK_EQ(ProxyFromSettings("http=hp:80", "", "https", "a.com"), std::string());
}

TEST_CASE(SystemProxy_Bypass) {
    const char* bypass = "*.corp.example; 10.*;intranet;<local>";
    CHECK_EQ(ProxyFromSettings("p:1", bypass, "http", "a.CORP.example"), std::string());
    CHECK_EQ(ProxyFromSettings("p:1", bypass, "http", "10.1.2.3"), std::string());
    CHECK_EQ(ProxyFromSettings("p:1", bypass, "http", "intranet"), std::string());
    CHECK_EQ(ProxyFromSettings("p:1", bypass, "http", "nas"), std::string());
    CHECK_EQ(ProxyFromSettings("p:1", bypass, "http", "corp.example"), std::string("p:1"));
    CHECK_EQ(ProxyFromSettings("p:1", bypass, "http", "110.1.2.3"), std::string("p:1"));
    CHECK_EQ(ProxyFromSettings("p:1", "", "http", "nas"), std::string("p:1"));}
