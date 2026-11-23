// SPDX-License-Identifier: GPL-3.0-only
// Diagnostic-only readback on the Windows software D3D12 adapter. These tests
// validate real shader pixels, not performance or RX 7900 XT compatibility.
#include "veyra/pipeline/CapturePredictionPass.h"
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace veyra::pipeline;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
namespace {
void require(bool ok,const char* what) { if(!ok)throw std::runtime_error(what); }
void hr(HRESULT value,const char* what) { require(SUCCEEDED(value),what); }
constexpr unsigned W=128,H=72,FW=64,FH=36;
float pattern(float x,float y) {
    return .35f+.12f*std::sin(x*.17f)+.08f*std::cos(y*.21f)+.05f*std::sin((x+y)*.31f);
}
struct Test {
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence; ComPtr<ID3D12InfoQueue> debug;
    HANDLE event=nullptr; uint64_t value=0;
    StateTracker tracker;
    std::array<ComPtr<ID3D12Resource>,5> in;
    std::array<ComPtr<ID3D12Resource>,2> out;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    CapturePredictionPass pass;
    Test() {
        ComPtr<ID3D12Debug> layer;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer))))layer->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"DXGI factory");
        ComPtr<IDXGIAdapter> warp;hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP adapter");
        hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12 WARP device");
        device.As(&debug);
        D3D12_COMMAND_QUEUE_DESC desc{};hr(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"queue");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
        hr(list->Close(),"initial close");hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"fence event");
        in[0]=makeTexture(device.Get(),W,H,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
        in[1]=makeTexture(device.Get(),W,H,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
        in[2]=makeTexture(device.Get(),FW,FH,DXGI_FORMAT_R32G32_FLOAT,false);
        in[3]=makeTexture(device.Get(),FW,FH,DXGI_FORMAT_R32G32_FLOAT,false);
        in[4]=makeTexture(device.Get(),FW,FH,DXGI_FORMAT_R32_FLOAT,false);
        for(auto& t:out)t=makeTexture(device.Get(),W,H,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        require(pass.initialize(device.Get(),{in[0].Get(),in[1].Get(),in[2].Get(),in[3].Get(),in[4].Get()},
            {out[0].Get(),out[1].Get()}),"initialize GPU predictor");
    }
    ~Test(){if(event)CloseHandle(event);}
    void begin(){uploads.clear();hr(allocator->Reset(),"reset allocator");hr(list->Reset(allocator.Get(),nullptr),"reset list");}
    void finish(){
        hr(list->Close(),"close list");ID3D12CommandList* work[]={list.Get()};queue->ExecuteCommandLists(1,work);
        hr(queue->Signal(fence.Get(),++value),"signal");hr(fence->SetEventOnCompletion(value,event),"set event");
        require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"bounded diagnostic wait");
        hr(device->GetDeviceRemovedReason(),"device health");
    }
    void upload(ID3D12Resource* texture,const std::vector<uint8_t>& bytes) {
        auto d=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows;UINT64 rowBytes,total;
        device->GetCopyableFootprints(&d,0,1,0,&footprint,&rows,&rowBytes,&total);
        require(bytes.size()==rowBytes*rows,"upload shape");
        auto buffer=makeUploadBuffer(device.Get(),total);require(bool(buffer),"upload allocation");
        void* mapped;hr(buffer->Map(0,nullptr,&mapped),"upload map");
        for(UINT y=0;y<rows;++y)std::memcpy(static_cast<uint8_t*>(mapped)+y*footprint.Footprint.RowPitch,bytes.data()+y*rowBytes,size_t(rowBytes));
        buffer->Unmap(0,nullptr);tracker.transition(list.Get(),texture,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=texture;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource=buffer.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);uploads.push_back(buffer);
    }
    template<class T> void put(ID3D12Resource* texture,const std::vector<T>& a) {
        std::vector<uint8_t> b(a.size()*sizeof(T));std::memcpy(b.data(),a.data(),b.size());upload(texture,b);
    }
    std::vector<uint16_t> colors(float shift,bool cut=false) {
        std::vector<uint16_t> p(W*H*4);
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
            float v=cut?3.f:pattern(float(x)-shift,float(y));
            for(unsigned c=0;c<3;++c)p[(y*W+x)*4+c]=XMConvertFloatToHalf(v);
            p[(y*W+x)*4+3]=XMConvertFloatToHalf(1);
        }return p;
    }
    std::vector<uint16_t> run(CapturePredictionPass::Parameters params,float shift=4,float confidence=1,
                             bool cut=false,bool badMotion=false,bool stop=false,unsigned output=0) {
        begin();put(in[0].Get(),colors(shift,cut));put(in[1].Get(),colors(0));
        std::vector<float> motion(FW*FH*2),old(FW*FH*2),conf(FW*FH,confidence);
        for(unsigned i=0;i<FW*FH;++i){motion[i*2]=badMotion?std::numeric_limits<float>::quiet_NaN():(stop?0:-shift*FW/W);old[i*2]=-shift*FW/W;}
        put(in[2].Get(),motion);put(in[3].Get(),old);put(in[4].Get(),conf);
        require(pass.record(list.Get(),tracker,output,params),"record forecast");
        auto d=out[output]->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows;UINT64 rowBytes,total;
        device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&rowBytes,&total);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=total;
        bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb)),"readback");
        tracker.transition(list.Get(),out[output].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=rb.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;
        from.pResource=out[output].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);finish();
        void* mapped;D3D12_RANGE range{0,size_t(total)};hr(rb->Map(0,&range,&mapped),"readback map");
        std::vector<uint16_t> result(W*H*4);
        for(UINT y=0;y<rows;++y)std::memcpy(result.data()+size_t(y)*W*4,static_cast<uint8_t*>(mapped)+y*fp.Footprint.RowPitch,size_t(rowBytes));
        D3D12_RANGE empty{0,0};rb->Unmap(0,&empty);return result;
    }
    void checkDebug() {
        if(!debug){std::cout<<"D3D12 debug layer unavailable\n";return;}
        for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T n=0;debug->GetMessage(i,nullptr,&n);
            std::vector<uint8_t> b(n);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(b.data());debug->GetMessage(i,msg,&n);
            if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<msg->pDescription<<'\n';require(false,"D3D12 validation error");}}
    }
};
}
int main() try {
    Test t;unsigned checks=0;
    auto check=[&](bool ok,const char* what){++checks;require(ok,what);};
    CapturePredictionPass::Parameters p;p.historyValid=true;p.horizonFraction=.5f;p.accelerationValid=true;
    const auto got=t.run(p),latest=t.colors(4),truth=t.colors(6);
    double originalError=0,predictionError=0;unsigned pixels=0;
    for(unsigned y=8;y<H-8;++y)for(unsigned x=16;x<W-16;++x){unsigned i=(y*W+x)*4;
        originalError+=std::abs(XMConvertHalfToFloat(latest[i])-XMConvertHalfToFloat(truth[i]));
        predictionError+=std::abs(XMConvertHalfToFloat(got[i])-XMConvertHalfToFloat(truth[i]));++pixels;}
    check(predictionError<originalError*.1,"forecast must match independently rendered future truth, not repeat input");
    check(got!=latest,"nonzero predictor changes real shader pixels");
    check(t.run(p,4,0)==latest,"zero confidence is exact latest-frame passthrough");
    check(t.run(p,4,1,true)==t.colors(4,true),"scene cut is exact latest-frame passthrough");
    check(t.run(p,4,1,false,true)==latest,"NaN motion cannot poison output");
    check(t.run(p,0,1,false,false,true)==t.colors(0),"stationary picture remains exact");
    p.horizonFraction=0;check(t.run(p)==latest,"zero lead is bit-exact source");p.horizonFraction=.5f;
    p.historyValid=false;check(t.run(p)==latest,"reset never consumes stale history");p.historyValid=true;
    p.protectionFeatherPixels=0;p.protectedRegions[0]={.25f,.25f,.75f,.75f};
    auto protectedImage=t.run(p);
    for(unsigned y=H/4;y<H*3/4;++y)for(unsigned x=W/4;x<W*3/4;++x)
        for(unsigned c=0;c<4;++c)check(protectedImage[(y*W+x)*4+c]==latest[(y*W+x)*4+c],"protected HUD remains exact");
    p.protectedRegions={};check(t.run(p,4,1,false,false,false,1)==got,"separate output slot is equivalent");
    for(unsigned y=0;y<H;++y)check(got[y*W*4]==latest[y*W*4],"exposed border falls back to source");
    auto invalid=p;invalid.horizonFraction=std::numeric_limits<float>::quiet_NaN();
    t.begin();check(!t.pass.record(t.list.Get(),t.tracker,0,invalid),"invalid parameters rejected before dispatch");t.finish();
    t.checkDebug();
    std::cout<<"Capture prediction D3D12 WARP: "<<checks<<" checks, 0 failures; synthetic known-motion MAE "
        <<originalError/pixels<<" -> "<<predictionError/pixels<<". Not hardware latency or estimated-flow quality.\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
