/* SPDX-License-Identifier: MIT */
/* Production picker, controlled SetupAPI boundary; no device installation. */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
using DWORD=uint32_t; using HDEVINFO=int;
constexpr DWORD MAX_PATH=260, DI_ENUMSINGLEINF=0x10000, DI_FLAGSEX_ALLOWEXCLUDEDDRVS=0x800;
constexpr DWORD INVALID_FILE_ATTRIBUTES=0xffffffff, FILE_ATTRIBUTE_DIRECTORY=0x10, SPDIT_COMPATDRIVER=2;
constexpr auto kOurProvider=L"XM6 Audio Project";
struct SP_DEVINFO_DATA {};
struct SP_DEVINSTALL_PARAMS_W { DWORD cbSize{},Flags{},FlagsEx{}; wchar_t DriverPath[MAX_PATH]{}; };
struct SP_DRVINFO_DATA_W { DWORD cbSize{}; wchar_t ProviderName[128]{}; uint64_t DriverDate{},DriverVersion{}; int identity{}; };
static SP_DEVINSTALL_PARAMS_W saved;
static std::wstring module=L"C:\\Store\\new\\oa2dpctl.exe",expected=L"C:\\Store\\new\\oa2dp.inf";
static DWORD attributes=0;
static bool set_ok=true;
static unsigned builds;
static std::vector<SP_DRVINFO_DATA_W> candidates,selected;
#ifndef _WIN32
static int _wcsicmp(const wchar_t*a,const wchar_t*b){return std::wcscmp(a,b);}
#endif
static int CompareFileTime(const uint64_t*a,const uint64_t*b){return *a==*b?0:(*a>*b?1:-1);}
static bool SetupDiGetDeviceInstallParamsW(HDEVINFO,SP_DEVINFO_DATA*,SP_DEVINSTALL_PARAMS_W*p){*p=saved;return true;}
static bool SetupDiSetDeviceInstallParamsW(HDEVINFO,SP_DEVINFO_DATA*,SP_DEVINSTALL_PARAMS_W*p){if(!set_ok)return false;saved=*p;return true;}
static DWORD GetModuleFileNameW(void*,wchar_t*p,DWORD size){if(module.size()>=size)return size;std::copy(module.begin(),module.end(),p);p[module.size()]=0;return static_cast<DWORD>(module.size());}
static DWORD GetFileAttributesW(const wchar_t*p){REQUIRE(p==expected);return attributes;}
static bool SetupDiBuildDriverInfoList(HDEVINFO,SP_DEVINFO_DATA*,DWORD type){
 REQUIRE(type==SPDIT_COMPATDRIVER);++builds;selected.clear();
 for(const auto&d:candidates) if(!(saved.Flags&DI_ENUMSINGLEINF)||d.identity==2)selected.push_back(d);
 if(saved.Flags&DI_ENUMSINGLEINF)REQUIRE(saved.DriverPath==expected);
 return true;
}
static bool SetupDiEnumDriverInfoW(HDEVINFO,SP_DEVINFO_DATA*,DWORD,DWORD i,SP_DRVINFO_DATA_W*d){if(i>=selected.size())return false;*d=selected[i];return true;}
#include "driver-selection-under-test.inc"
static SP_DRVINFO_DATA_W driver(const wchar_t*provider,int id,uint64_t version){SP_DRVINFO_DATA_W d{};std::wcscpy(d.ProviderName,provider);d.DriverDate=10;d.DriverVersion=version;d.identity=id;return d;}
int main(){
 SP_DEVINFO_DATA dev;SP_DRVINFO_DATA_W chosen{};
 candidates={driver(kOurProvider,1,400),driver(kOurProvider,2,400),driver(L"Microsoft",3,600)};
 REQUIRE(pick_driver(0,dev,true,&chosen)&&chosen.identity==2); // tied old package must not win
 REQUIRE(pick_driver(0,dev,false,&chosen)&&chosen.identity==3); // clear previous single-INF scope
 attributes=INVALID_FILE_ATTRIBUTES;builds=0;
 REQUIRE(!pick_driver(0,dev,true,&chosen)&&builds==0); // no fallback to unrelated installed candidate
 attributes=FILE_ATTRIBUTE_DIRECTORY;REQUIRE(!pick_driver(0,dev,true,&chosen)&&builds==0);
 attributes=0;set_ok=false;REQUIRE(!pick_driver(0,dev,true,&chosen)&&builds==0);
 set_ok=true;module.assign(MAX_PATH,L'x');REQUIRE(!pick_driver(0,dev,true,&chosen)&&builds==0);
 std::puts("Native driver selection uses its exact bundled INF; tied packages, recovery and boundary failures passed.");
}
