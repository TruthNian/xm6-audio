/* SPDX-License-Identifier: MIT */
/* Production output drain with controlled COM ownership. No real codec/device.
 * In particular, a returned empty collection is owned even without media. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
#define SUCCEEDED(x) ((x)>=0)
#define FAILED(x) ((x)<0)
using HRESULT=int32_t;
using DWORD=uint32_t;
using BYTE=uint8_t;
constexpr HRESULT S_OK=0,E_FAIL=-1,MF_E_TRANSFORM_NEED_MORE_INPUT=-2,E_UNEXPECTED=-3;
constexpr DWORD MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE=0x300;
static unsigned live[3];
struct Ref {
    unsigned kind,refs=1;
    explicit Ref(unsigned k):kind(k){++live[k];}
    virtual ~Ref(){--live[kind];}
    void AddRef(){++refs;}
    void Release(){REQUIRE(refs);if(!--refs)delete this;}
};
struct IMFCollection:Ref { unsigned count;explicit IMFCollection(unsigned n):Ref(2),count(n){} };
struct IMFMediaBuffer:Ref {
    std::vector<BYTE> data;
    bool fail_lock=false;
    IMFMediaBuffer():Ref(0){}
    HRESULT Lock(BYTE **out,DWORD *,DWORD *length){if(fail_lock)return E_FAIL;*out=data.data();*length=DWORD(data.size());return S_OK;}
    HRESULT Unlock(){return S_OK;}
};
static bool fail_sample,fail_buffer,fail_add;
struct IMFSample:Ref {
    IMFMediaBuffer *buffer=nullptr;
    bool fail_contiguous=false;
    IMFSample():Ref(1){}
    ~IMFSample(){if(buffer)buffer->Release();}
    HRESULT AddBuffer(IMFMediaBuffer *b){if(fail_add)return E_FAIL;REQUIRE(!buffer);buffer=b;b->AddRef();return S_OK;}
    HRESULT ConvertToContiguousBuffer(IMFMediaBuffer **out){if(fail_contiguous)return E_FAIL;REQUIRE(buffer);buffer->AddRef();*out=buffer;return S_OK;}
};
static HRESULT MFCreateSample(IMFSample **out){if(fail_sample)return E_FAIL;*out=new IMFSample;return S_OK;}
static HRESULT MFCreateMemoryBuffer(DWORD,IMFMediaBuffer **out){if(fail_buffer)return E_FAIL;*out=new IMFMediaBuffer;return S_OK;}
struct MFT_OUTPUT_DATA_BUFFER { IMFSample *pSample=nullptr;IMFCollection *pEvents=nullptr;DWORD dwStatus=0; };
struct Step {HRESULT result;bool collection,media,contiguous_failure=false,lock_failure=false,empty_media=false;};
struct IMFTransform {
    bool provides;
    std::deque<Step> steps;
    explicit IMFTransform(bool p):provides(p){}
    HRESULT ProcessOutput(DWORD,DWORD count,MFT_OUTPUT_DATA_BUFFER *out,DWORD *){
        REQUIRE(count==1);
        if(steps.empty())return MF_E_TRANSFORM_NEED_MORE_INPUT;
        const Step s=steps.front();steps.pop_front();
        if(s.collection)out->pEvents=new IMFCollection(s.result==S_OK?1:0);
        if(!s.media)out->dwStatus=MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE;
        if(s.media){
            REQUIRE(s.result==S_OK);
            if(provides){
                out->pSample=new IMFSample;
                auto *buffer=new IMFMediaBuffer;
                REQUIRE(out->pSample->AddBuffer(buffer)==S_OK);
                buffer->Release();
            }
            REQUIRE(out->pSample && out->pSample->buffer);
            out->pSample->buffer->data=s.empty_media?std::vector<BYTE>{}:std::vector<BYTE>{1,2,3};
            out->pSample->fail_contiguous=s.contiguous_failure;
            out->pSample->buffer->fail_lock=s.lock_failure;
        }
        return s.result;
    }
};
struct AacState {
    IMFTransform *mft;
    bool mft_allocates;
    DWORD out_buffer_size=32;
    std::deque<std::vector<uint8_t>> pending;
};
#include "aac-release-under-test.inc"
#include "aac-output-under-test.inc"
static void balanced(){REQUIRE(live[0]==0 && live[1]==0 && live[2]==0);}
static void events_only(){
    for(bool provides:{false,true}){
        IMFTransform mft(provides);
        mft.steps.push_back({S_OK,true,false});
        AacState state{&mft,provides};
        REQUIRE(drain(&state)==S_OK);
        if(!state.pending.empty())std::fprintf(stderr,"events-only completion queued %zu bytes as AAC media\n",state.pending.front().size());
        REQUIRE(state.pending.empty());
        balanced();
    }
}
int main(int argc,char **argv){
    if(argc==2 && !std::strcmp(argv[1],"--events-only")){events_only();return 0;}
    for(bool provides:{false,true}){
        {
            IMFTransform mft(provides);
            mft.steps.push_back({MF_E_TRANSFORM_NEED_MORE_INPUT,true,false});
            AacState state{&mft,provides};
            REQUIRE(drain(&state)==S_OK && state.pending.empty());
            balanced(); // old early return leaves the empty collection alive
        }
        {
            IMFTransform mft(provides);
            mft.steps.push_back({S_OK,true,true});
            mft.steps.push_back({MF_E_TRANSFORM_NEED_MORE_INPUT,true,false});
            AacState state{&mft,provides};
            REQUIRE(drain(&state)==S_OK && state.pending.size()==1);
            REQUIRE((state.pending.front()==std::vector<uint8_t>{1,2,3}));
            balanced();
        }
        for(unsigned error=0;error<4;++error){
            IMFTransform mft(provides);
            mft.steps.push_back({error?S_OK:E_FAIL,true,error!=0,error==1,error==2,error==3});
            AacState state{&mft,provides};
            REQUIRE(drain(&state)==(error==3?E_UNEXPECTED:E_FAIL) && state.pending.empty());
            balanced();
        }
    }
    events_only();
    for(unsigned error=0;error<3;++error){
        fail_sample=error==0;fail_buffer=error==1;fail_add=error==2;
        IMFTransform mft(false);AacState state{&mft,false};
        REQUIRE(drain(&state)==E_FAIL);balanced();
    }
    std::puts("production AAC output cleanup preserves ownership across input-needed, allocation, output and buffer errors");
}
