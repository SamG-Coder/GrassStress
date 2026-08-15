#include "dxr_renderer.hpp"
#include "first_person_camera.hpp"
#include "gpu_telemetry.hpp"
#include "grass_field.hpp"
#include "optix_denoise.hpp"
#include "rtx_caps.hpp"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t windowClassName[] = L"GrassStressWindow";

enum class CameraMode {
    Cinematic,
    Orbit,
    FirstPerson
};

struct CinematicKey {
    float time;
    dense::Vec3 eye;
    dense::Vec3 target;
};

float wrapHours(float hours) {
    hours=std::fmod(hours,24.0f);
    return hours<0.0f?hours+24.0f:hours;
}

dense::Vec3 smoothstepVec(dense::Vec3 a,dense::Vec3 b,float t) {
    t=dense::clamp(t,0.0f,1.0f);
    t=t*t*(3.0f-2.0f*t);
    return dense::lerp(a,b,t);
}

class App {
public:
    dense::DxrRenderer renderer;
    dense::EnvironmentSimulation environment;
    dense::FirstPersonCameraController firstPerson;
    dense::GpuCapabilities gpu;
    dense::DebugRenderSettings debugSettings;
    dense::EnvironmentMesh field;
    std::uint32_t bladeCount=grass::kTargetBlades;
    std::uint32_t patchCount{};

    CameraMode cameraMode=CameraMode::Cinematic;
    bool hudVisible=true;
    bool vsync=false;
    bool dragging=false;
    bool firstPersonCaptured=false;
    bool moveForward=false,moveBackward=false,moveLeft=false,moveRight=false;
    bool sprint=false,jump=false;
    float pendingYaw=0,pendingPitch=0;
    float yaw=.62f,pitch=.22f,distance=6.5f;
    POINT last{};
    dense::Vec3 grassWake{};
    dense::Vec3 orbitTarget{0.0f,0.35f,0.0f};

    float cinematicTime=0.0f;
    float titleAge=0.0f;
    double smoothedMs=16.0;
    std::array<float,180> frameMsHistory{};
    std::size_t frameMsCount=0;
    dense::GpuTelemetry telemetry{};
    int telemetryAge=0;
    std::chrono::steady_clock::time_point lastCameraUpdate{};
    std::chrono::steady_clock::time_point lastEnvironmentUpdate{};
    HWND window{};

    App()
        :firstPerson(makeWalkSettings(),
            [](float x,float z){return grass::sampleTerrain(x,z);}) {
        debugSettings.grassDensity=1.0f;
        debugSettings.bladeHeightScale=1.22f;
        debugSettings.shortGrassDrawDistance=std::numeric_limits<float>::max();
        debugSettings.tallGrassDrawDistance=std::numeric_limits<float>::max();
        environment.controls.advanceTime=true;
        environment.controls.dayLengthSeconds=40.0f;
        environment.controls.timeScale=1.0f;
        environment.state.timeOfDay=15.25f;
        environment.controls.sunIntensityScale=2.15f;
        environment.controls.moonIntensityScale=2.85f;
        environment.controls.windSpeed=3.45f;
        environment.controls.windStrength=1.72f;
        environment.controls.windGustFrequency=1.95f;
        environment.controls.baseFogDensity=0.00078f;
        environment.update(0.0f);
        firstPerson.reset(-8.5f,14.0f,-.18f,-.06f);
    }

    void buildField() {
        field=grass::build(5080);
        patchCount=static_cast<std::uint32_t>(field.grassPatches.size());
        bladeCount=grass::countedBlades(field);
    }

    static dense::FirstPersonCameraSettings makeWalkSettings() {
        dense::FirstPersonCameraSettings settings;
        settings.horizontalHalfExtent=grass::kTerrainHalfZ-3.0f;
        settings.eyeHeight=1.62f;
        return settings;
    }

    void captureMouse() {
        if(cameraMode!=CameraMode::FirstPerson||!window)return;
        firstPersonCaptured=true;
        SetCapture(window);
        RECT client{};
        GetClientRect(window,&client);
        POINT a{client.left,client.top},b{client.right,client.bottom};
        ClientToScreen(window,&a);ClientToScreen(window,&b);
        RECT clip{a.x,a.y,b.x,b.y};
        ClipCursor(&clip);
        SetCursor(nullptr);
        lastCameraUpdate=std::chrono::steady_clock::now();
    }

    void releaseMouse() {
        firstPersonCaptured=false;
        moveForward=moveBackward=moveLeft=moveRight=sprint=jump=false;
        pendingYaw=pendingPitch=0;
        ClipCursor(nullptr);
        if(window&&GetCapture()==window)ReleaseCapture();
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    }

    void setMode(CameraMode mode) {
        if(mode==cameraMode)return;
        cameraMode=mode;
        if(mode==CameraMode::FirstPerson)captureMouse();
        else releaseMouse();
    }

    dense::CameraView cinematicView(float elapsed) {
        cinematicTime+=elapsed;
        constexpr float loop=48.0f;
        if(cinematicTime>=loop)cinematicTime-=loop;
        const float t=cinematicTime;
        // Standing height over the meadow, looking into the blades. Low enough
        // that ribbons stay visible; high enough that the frustum is not
        // buried in the dirt.
        const float yaw=t*.16f;
        const float x=3.4f*std::sin(yaw)+.7f*std::sin(t*.12f);
        const float z=4.2f*std::cos(yaw*.94f)+.5f*std::cos(t*.09f);
        const float look=yaw+1.22f+.10f*std::sin(t*.23f);
        const float reach=5.4f+.4f*std::sin(t*.14f);
        const float tx=x+std::sin(look)*reach;
        const float tz=z+std::cos(look)*reach;
        dense::Vec3 eye{x,grass::terrainHeight(x,z)+1.82f,z};
        dense::Vec3 target{tx,grass::terrainHeight(tx,tz)+.22f,tz};
        return {eye,dense::normalize(target-eye)};
    }

    void updateCamera(float elapsed) {
        if(cameraMode==CameraMode::Cinematic)return;
        if(cameraMode==CameraMode::Orbit) {
            grassWake={};
            return;
        }
        dense::FirstPersonCameraInput input{};
        if(firstPersonCaptured) {
            input.forward=moveForward;
            input.backward=moveBackward;
            input.left=moveLeft;
            input.right=moveRight;
            input.sprint=sprint;
            input.jump=jump;
            input.yawDelta=pendingYaw;
            input.pitchDelta=pendingPitch;
        }
        pendingYaw=pendingPitch=0;
        firstPerson.update(elapsed,input);
        const dense::Vec3 target=firstPerson.state().horizontalVelocity;
        const float response=dense::lengthSq(target)>.01f?14.0f:3.2f;
        const float blend=1.0f-std::exp(-response*std::min(elapsed,.10f));
        grassWake=dense::lerp(grassWake,target,blend);
    }

    dense::CameraView cameraView() const {
        if(cameraMode==CameraMode::FirstPerson) {
            const auto pose=firstPerson.pose();
            dense::CameraView view{pose.eye,pose.forward};
            view.grassInteractionPosition=firstPerson.state().footPosition;
            view.grassInteractionVelocity=grassWake;
            view.grassInteractionEnabled=true;
            return view;
        }
        if(cameraMode==CameraMode::Cinematic)
            return const_cast<App*>(this)->cinematicView(0.0f);
        dense::Vec3 eye=orbitTarget+dense::Vec3{
            std::sin(yaw)*std::cos(pitch)*distance,
            std::sin(pitch)*distance,
            -std::cos(yaw)*std::cos(pitch)*distance};
        eye.y=std::max(eye.y,grass::terrainHeight(eye.x,eye.z)+.28f);
        return {eye,dense::normalize(orbitTarget-eye)};
    }

    bool setMoveKey(WPARAM key,bool pressed) {
        bool* state=nullptr;
        switch(key) {
        case 'W':state=&moveForward;break;
        case 'S':state=&moveBackward;break;
        case 'A':state=&moveLeft;break;
        case 'D':state=&moveRight;break;
        case VK_SHIFT:state=&sprint;break;
        case VK_SPACE:state=&jump;break;
        default:return false;
        }
        if(cameraMode==CameraMode::FirstPerson)
            *state=firstPersonCaptured&&pressed;
        else if(!pressed)*state=false;
        return cameraMode==CameraMode::FirstPerson;
    }

    void handleRawMouse(HRAWINPUT handle) {
        if(cameraMode!=CameraMode::FirstPerson||!firstPersonCaptured)return;
        UINT bytes=0;
        if(GetRawInputData(handle,RID_INPUT,nullptr,&bytes,sizeof(RAWINPUTHEADER))!=0)
            return;
        std::vector<unsigned char> storage(bytes);
        if(GetRawInputData(handle,RID_INPUT,storage.data(),&bytes,
                           sizeof(RAWINPUTHEADER))!=bytes)return;
        const auto* raw=reinterpret_cast<const RAWINPUT*>(storage.data());
        if(raw->header.dwType!=RIM_TYPEMOUSE||
           (raw->data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)!=0)return;
        pendingYaw+=static_cast<float>(raw->data.mouse.lLastX)*.0022f;
        pendingPitch+=static_cast<float>(raw->data.mouse.lLastY)*.0022f;
    }

    void toggleFullscreen() {
        static WINDOWPLACEMENT placement{};
        static bool fullscreen=false;
        if(!window)return;
        if(!fullscreen) {
            placement.length=sizeof(placement);
            GetWindowPlacement(window,&placement);
            MONITORINFO monitor{};
            monitor.cbSize=sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
            SetWindowLongW(window,GWL_STYLE,WS_POPUP|WS_VISIBLE);
            SetWindowPos(window,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,
                         monitor.rcMonitor.right-monitor.rcMonitor.left,
                         monitor.rcMonitor.bottom-monitor.rcMonitor.top,
                         SWP_FRAMECHANGED|SWP_SHOWWINDOW);
            fullscreen=true;
        } else {
            SetWindowLongW(window,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE);
            SetWindowPlacement(window,&placement);
            SetWindowPos(window,nullptr,0,0,0,0,
                         SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
            fullscreen=false;
        }
    }

    void noteFrame(float frameMs) {
        if(frameMsCount<frameMsHistory.size())
            frameMsHistory[frameMsCount++]=frameMs;
        else {
            std::copy(frameMsHistory.begin()+1,frameMsHistory.end(),frameMsHistory.begin());
            frameMsHistory.back()=frameMs;
        }
    }

    float percentileMs(float fraction) const {
        if(frameMsCount==0)return 0.0f;
        std::array<float,180> sorted=frameMsHistory;
        std::sort(sorted.begin(),sorted.begin()+static_cast<std::ptrdiff_t>(frameMsCount));
        const std::size_t index=std::min(frameMsCount-1,
            static_cast<std::size_t>(fraction*static_cast<float>(frameMsCount)));
        return sorted[index];
    }

    void pushHud(float fps,float frameMs) {
        if(++telemetryAge>=8) {
            telemetry=dense::sampleGpuTelemetry();
            telemetryAge=0;
        }
        dense::HudState hud;
        hud.visible=hudVisible;
        hud.titleAlpha=hudVisible?dense::clamp(1.0f-titleAge/6.0f,0.0f,1.0f):0.0f;
        hud.hudAlpha=hudVisible?1.0f:0.0f;
        hud.fps=fps;
        hud.frameMs=frameMs;
        hud.frameMsP1=percentileMs(.99f);
        hud.frameMsMax=percentileMs(1.0f);
        hud.gpuTempC=telemetry.temperatureC;
        hud.gpuUtil=telemetry.utilizationPercent;
        hud.powerW=telemetry.powerWatts;
        hud.vramUsedGiB=telemetry.vramUsedGiB;
        hud.clockMHz=telemetry.clockMHz;
        const auto&stream=renderer.streamStatus();
        const float presented=static_cast<float>(std::max<std::uint32_t>(1,stream.presentedFrames));
        hud.displayFps=fps*presented;
        hud.displayFrameMs=hud.displayFps>0.05f?1000.0f/hud.displayFps:frameMs;
        hud.mfgMultiplier=stream.mfgMultiplier;
        hud.dlssMode=stream.rayReconstruction&&stream.quality!=dense::DlssQuality::Off?2u:
                     (stream.dlss&&stream.quality!=dense::DlssQuality::Off?1u:0u);
        hud.experiment=renderer.experiment();
        hud.choking=fps<20.0f||hud.frameMsP1>std::max(48.0f,hud.frameMs*2.2f);
        hud.blades=bladeCount;
        hud.patches=patchCount;
        hud.timeOfDay=environment.state.timeOfDay;
        hud.cinematic=cameraMode==CameraMode::Cinematic;
        const std::wstring& name=gpu.adapter;
        const size_t n=std::min<size_t>(name.size(),63);
        for(size_t i=0;i<n;++i)
            hud.gpuName[i]=static_cast<char>(name[i]<128?name[i]:'?');
        renderer.setHud(hud);
    }
};

App* app=nullptr;

LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(!app)return DefWindowProcW(window,message,wParam,lParam);
    switch(message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(window,&ps);
        RECT rc{};GetClientRect(window,&rc);
        FillRect(dc,&rc,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,RGB(200,200,190));
        DrawTextW(dc,L"Building 60,000,000 path-traced grass blades...",-1,&rc,
                  DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        EndPaint(window,&ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if(wParam!=SIZE_MINIMIZED&&app->renderer.ready())
            app->renderer.resize(LOWORD(lParam),HIWORD(lParam));
        return 0;
    case WM_LBUTTONDOWN:
        if(app->cameraMode==CameraMode::Orbit) {
            app->dragging=true;
            app->last={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            SetCapture(window);
        } else if(app->cameraMode==CameraMode::FirstPerson&&!app->firstPersonCaptured)
            app->captureMouse();
        return 0;
    case WM_LBUTTONUP:
        if(app->dragging) {
            app->dragging=false;
            if(GetCapture()==window)ReleaseCapture();
        }
        return 0;
    case WM_MOUSEMOVE:
        if(app->dragging&&app->cameraMode==CameraMode::Orbit) {
            const int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
            app->yaw+=(x-app->last.x)*.005f;
            app->pitch=dense::clamp(app->pitch+(y-app->last.y)*.005f,-1.2f,1.35f);
            app->last={x,y};
        }
        return 0;
    case WM_MOUSEWHEEL:
        if(app->cameraMode==CameraMode::Orbit) {
            const float delta=GET_WHEEL_DELTA_WPARAM(wParam)/120.0f;
            app->distance=dense::clamp(app->distance-delta*2.4f,2.0f,90.0f);
        }
        return 0;
    case WM_INPUT:
        app->handleRawMouse(reinterpret_cast<HRAWINPUT>(lParam));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if((lParam&(1<<30))!=0) {
            app->setMoveKey(wParam,true);
            return 0;
        }
        if(app->setMoveKey(wParam,true))return 0;
        switch(wParam) {
        case VK_ESCAPE:
            if(app->cameraMode==CameraMode::FirstPerson&&app->firstPersonCaptured)
                app->releaseMouse();
            else PostQuitMessage(0);
            return 0;
        case VK_F2:
            app->setMode(app->cameraMode==CameraMode::FirstPerson?
                         CameraMode::Orbit:CameraMode::FirstPerson);
            return 0;
        case 'C':
            app->setMode(app->cameraMode==CameraMode::Cinematic?
                         CameraMode::Orbit:CameraMode::Cinematic);
            return 0;
        case 'H':
        case VK_F1:
            app->hudVisible=!app->hudVisible;
            return 0;
        case 'V':
            app->vsync=!app->vsync;
            app->renderer.setVsync(app->vsync);
            return 0;
        case 'E':
            app->renderer.setExperiment(app->renderer.experiment()+1);
            return 0;
        case 'D': {
            const auto current=app->renderer.streamStatus().quality;
            dense::DlssQuality next=dense::DlssQuality::Off;
            if(current==dense::DlssQuality::Off)next=dense::DlssQuality::Quality;
            else if(current==dense::DlssQuality::Quality)next=dense::DlssQuality::Balanced;
            else if(current==dense::DlssQuality::Balanced)next=dense::DlssQuality::Performance;
            else if(current==dense::DlssQuality::Performance)next=dense::DlssQuality::UltraPerformance;
            else if(current==dense::DlssQuality::UltraPerformance)next=dense::DlssQuality::Dlaa;
            app->renderer.setDlssQuality(next);
            return 0;
        }
        case 'F': {
            const auto current=app->renderer.streamStatus().frameGen;
            dense::FrameGenMode next=dense::FrameGenMode::Off;
            if(current==dense::FrameGenMode::Off)next=dense::FrameGenMode::X2;
            else if(current==dense::FrameGenMode::X2)next=dense::FrameGenMode::X4;
            else if(current==dense::FrameGenMode::X4)next=dense::FrameGenMode::X6;
            else if(current==dense::FrameGenMode::X6)next=dense::FrameGenMode::Dynamic;
            app->renderer.setFrameGenMode(next);
            return 0;
        }
        case VK_F11:
            app->toggleFullscreen();
            return 0;
        case 'R':
            app->cinematicTime=0;
            app->titleAge=0;
            app->setMode(CameraMode::Cinematic);
            return 0;
        case '1':
            app->environment.state.timeOfDay=7.15f;
            return 0;
        case '2':
            app->environment.state.timeOfDay=13.0f;
            return 0;
        case '3':
            app->environment.state.timeOfDay=18.35f;
            return 0;
        case '4':
            app->environment.state.timeOfDay=21.4f;
            return 0;
        case VK_LEFT:
            app->environment.state.timeOfDay=
                wrapHours(app->environment.state.timeOfDay-.35f);
            return 0;
        case VK_RIGHT:
            app->environment.state.timeOfDay=
                wrapHours(app->environment.state.timeOfDay+.35f);
            return 0;
        case 'W':
            if(app->cameraMode!=CameraMode::FirstPerson) {
                if(app->environment.controls.windStrength<.05f)
                    app->environment.controls.windStrength=1.12f;
                else app->environment.controls.windStrength=0.0f;
            }
            return 0;
        default:break;
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        app->setMoveKey(wParam,false);
        return 0;
    case WM_CAPTURECHANGED:
        if(reinterpret_cast<HWND>(lParam)!=window) {
            app->firstPersonCaptured=false;
            app->dragging=false;
        }
        return 0;
    default:break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    CreateDirectoryW(L"C:\\StressTest\\video",nullptr);
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","w")){
        fputs("wWinMain\n",boot);fclose(boot);
    }
    SetProcessDPIAware();
    int argumentCount=0;
    wchar_t** arguments=CommandLineToArgvW(GetCommandLineW(),&argumentCount);
    bool recordMode=false;
    bool offlineMode=false;
    int benchFrames=0;
    int offlineSpp=16;
    int offlineShots=48;
    int startExperiment=0;
    for(int i=1;i<argumentCount;++i)
        if(arguments[i]&&(_wcsicmp(arguments[i],L"--record")==0||
                          _wcsicmp(arguments[i],L"--youtube")==0))
            recordMode=true;
        else if(arguments[i]&&_wcsicmp(arguments[i],L"--offline")==0) {
            offlineMode=true;
            recordMode=true;
            if(i+1<argumentCount&&arguments[i+1][0]!=L'-')
                offlineSpp=std::max(2,_wtoi(arguments[++i]));
            if(i+1<argumentCount&&arguments[i+1][0]!=L'-')
                offlineShots=std::max(1,_wtoi(arguments[++i]));
        }
        else if(arguments[i]&&_wcsicmp(arguments[i],L"--exp")==0) {
            if(i+1<argumentCount)startExperiment=std::max(0,_wtoi(arguments[++i]))%8;
        }
        else if(arguments[i]&&_wcsicmp(arguments[i],L"--bench")==0) {
            benchFrames=20;
            if(i+1<argumentCount)benchFrames=std::max(5,_wtoi(arguments[i+1]));
        }
    if(arguments)LocalFree(arguments);

    WNDCLASSEXW wc{};
    wc.cbSize=sizeof(wc);
    wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=windowProc;
    wc.hInstance=instance;
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName=windowClassName;
    if(!RegisterClassExW(&wc)){
        if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){fputs("register_class_failed\n",boot);fclose(boot);}
        return 1;
    }
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){fputs("class_ok\n",boot);fclose(boot);}

    const DWORD style=recordMode?WS_POPUP|WS_CLIPCHILDREN:
                                 WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;
    RECT rect{0,0,1920,1080};
    if(!recordMode)AdjustWindowRect(&rect,style,FALSE);
    int winW=rect.right-rect.left,winH=rect.bottom-rect.top;
    int winX=CW_USEDEFAULT,winY=CW_USEDEFAULT;
    if(recordMode) {
        const int screenW=GetSystemMetrics(SM_CXSCREEN);
        const int screenH=GetSystemMetrics(SM_CYSCREEN);
        winX=std::max(0,(screenW-1920)/2);
        winY=std::max(0,(screenH-1080)/2);
    }
    if(winX==CW_USEDEFAULT) {
        const int screenW=GetSystemMetrics(SM_CXSCREEN);
        const int screenH=GetSystemMetrics(SM_CYSCREEN);
        winX=std::max(0,(screenW-winW)/2);
        winY=std::max(0,(screenH-winH)/2);
    }
    HWND window=CreateWindowExW(0,windowClassName,
        L"Building 60,000,000 path-traced grass blades...",
        style|WS_VISIBLE,winX,winY,winW,winH,nullptr,nullptr,instance,nullptr);
    if(!window){
        if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){fputs("create_window_failed\n",boot);fclose(boot);}
        return 2;
    }
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){fputs("window_ok\n",boot);fclose(boot);}
    ShowWindow(window,SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);
    SetCursor(LoadCursor(nullptr,IDC_WAIT));
    {
        MSG pump{};
        while(PeekMessageW(&pump,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&pump);
            DispatchMessageW(&pump);
        }
    }
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
        fputs("creating_app\n",boot);fclose(boot);
    }
    auto owned=std::make_unique<App>();
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
        fputs("app_ready\n",boot);fclose(boot);
    }
    app=owned.get();
    app->window=window;
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
        fputs("skip_gpu_query\n",boot);fclose(boot);
    }
    app->gpu.adapter=L"NVIDIA";
    app->gpu.directX12=true;
    app->gpu.rayTracingTier=1;

    RAWINPUTDEVICE mouse{};
    mouse.usUsagePage=0x01;
    mouse.usUsage=0x02;
    mouse.hwndTarget=window;
    RegisterRawInputDevices(&mouse,1,sizeof(mouse));

    RECT client{};
    GetClientRect(window,&client);
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
        fprintf(boot,"initialize %d x %d\n",client.right,client.bottom);fclose(boot);
    }
    if(!app->renderer.initialize(window,client.right,client.bottom)) {
        MessageBoxW(window,app->renderer.error(),L"Grass Stress DXR error",MB_ICONERROR);
        return 3;
    }
    app->renderer.setVsync(false);
    app->renderer.setExperiment(static_cast<std::uint32_t>(startExperiment));
    SetWindowTextW(window,L"Building 60,000,000 path-traced grass blades...");
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
        fputs("building_grass\n",boot);fclose(boot);
    }
    {
        MSG pump{};
        while(PeekMessageW(&pump,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&pump);
            DispatchMessageW(&pump);
        }
    }
    app->buildField();
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
        fprintf(boot,"field_ok blades %u patches %u\n",app->bladeCount,app->patchCount);
        fclose(boot);
    }
    app->renderer.setWorld(std::move(app->field),
        [](float,float){return dense::PersistentWaterSample{};});
    app->renderer.setTree(dense::TreeMesh{});
    if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
        fputs("grass_ready\n",boot);fclose(boot);
    }
    if(!app->renderer.ready()) {
        MessageBoxW(window,app->renderer.error(),L"Grass Stress scene error",MB_ICONERROR);
        return 4;
    }

    std::wstringstream title;
    title<<L"RTX 5080 vs "<<app->bladeCount
         <<L" volumetric grass blades  —  path-traced bounces  —  "
         <<app->gpu.adapter;
    if(!recordMode)SetWindowTextW(window,title.str().c_str());
    else SetWindowTextW(window,L"GrassStress");
    SetCursor(recordMode?nullptr:LoadCursor(nullptr,IDC_ARROW));
    ShowWindow(window,show);
    UpdateWindow(window);
    if(recordMode) {
        SetForegroundWindow(window);
        SetCursor(nullptr);
        app->cameraMode=CameraMode::Cinematic;
        app->titleAge=0;
        app->cinematicTime=0;
        app->hudVisible=!offlineMode;
    }
    if(offlineMode) {
        app->environment.controls.windSpeed=0.0f;
        app->environment.controls.windStrength=0.0f;
        app->environment.controls.windGustFrequency=0.0f;
        app->environment.controls.advanceTime=false;
        app->environment.update(0.0f);
        app->renderer.setDlssQuality(dense::DlssQuality::Off);
        app->renderer.setOfflineAccumulate(static_cast<std::uint32_t>(offlineSpp));
        app->hudVisible=false;
        CreateDirectoryW(L"C:\\StressTest\\video\\offline",nullptr);
        if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
            fprintf(boot,"offline spp %d shots %d tensor %s\n",
                    offlineSpp,offlineShots,dense::tensorDenoiseStatus().label);
            fclose(boot);
        }
        bool cancelled=false;
        for(int shot=0;shot<offlineShots&&!cancelled;++shot) {
            // Bias shots toward the close meadow keys (0-22s), then the orbit.
            const float keys[]={2.4f,7.5f,14.0f,20.0f,26.0f,33.0f,40.0f,48.0f};
            if(offlineShots<=8)
                app->cinematicTime=keys[shot%8];
            else
                app->cinematicTime=(static_cast<float>(shot)+.5f)*(52.0f/static_cast<float>(offlineShots));
            app->renderer.setOfflineAccumulate(static_cast<std::uint32_t>(offlineSpp));
            for(int sample=0;sample<offlineSpp&&!cancelled;++sample) {
                MSG pump{};
                while(PeekMessageW(&pump,nullptr,0,0,PM_REMOVE)) {
                    if(pump.message==WM_QUIT){cancelled=true;break;}
                    TranslateMessage(&pump);
                    DispatchMessageW(&pump);
                }
                if(cancelled)break;
                const auto frameStart=std::chrono::steady_clock::now();
                app->renderer.render(app->cameraView(),app->debugSettings,
                                     app->environment.constants(),dense::PlayerLocalLight{});
                const double frameMs=std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-frameStart).count();
                app->smoothedMs=app->smoothedMs*.88+frameMs*.12;
                app->noteFrame(static_cast<float>(frameMs));
                const float fps=app->smoothedMs>0.05?static_cast<float>(1000.0/app->smoothedMs):0.0f;
                app->pushHud(fps,static_cast<float>(app->smoothedMs));
                wchar_t progress[160];
                swprintf(progress,160,L"Offline %d/%d  spp %d/%d  %.1f PT FPS",
                         shot+1,offlineShots,sample+1,offlineSpp,fps);
                SetWindowTextW(window,progress);
            }
            wchar_t pngPath[280];
            swprintf(pngPath,280,L"C:\\StressTest\\video\\offline\\frame_%04d.png",shot);
            app->renderer.writeDisplayPng(pngPath);
        }
        if(!cancelled) {
            wchar_t ffmpeg[MAX_PATH]{};
            if(SearchPathW(nullptr,L"ffmpeg.exe",nullptr,MAX_PATH,ffmpeg,nullptr)==0) {
                const wchar_t* winget=
                    L"C:\\Users\\samue\\AppData\\Local\\Microsoft\\WinGet\\Packages\\"
                    L"Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\\"
                    L"ffmpeg-9.0-full_build\\bin\\ffmpeg.exe";
                if(GetFileAttributesW(winget)!=INVALID_FILE_ATTRIBUTES)
                    wcsncpy(ffmpeg,winget,MAX_PATH-1);
            }
            if(ffmpeg[0]) {
                std::wstring cmd=L"\"";
                cmd+=ffmpeg;
                cmd+=L"\" -y -framerate 12 -i \"C:\\StressTest\\video\\offline\\frame_%04d.png\" "
                     L"-c:v h264_nvenc -preset p4 -tune hq -rc constqp -qp 17 "
                     L"-pix_fmt yuv420p -movflags +faststart "
                     L"\"C:\\StressTest\\video\\RTX5080-10M-Grass-OFFLINE.mp4\"";
                STARTUPINFOW si{};si.cb=sizeof(si);
                PROCESS_INFORMATION pi{};
                std::vector<wchar_t> mutableCmd(cmd.begin(),cmd.end());
                mutableCmd.push_back(0);
                if(CreateProcessW(nullptr,mutableCmd.data(),nullptr,nullptr,FALSE,
                                  CREATE_NO_WINDOW,nullptr,L"C:\\StressTest",
                                  &si,&pi)) {
                    WaitForSingleObject(pi.hProcess,INFINITE);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
            if(FILE*boot=fopen("C:\\StressTest\\video\\boot.txt","a")){
                fputs("offline_done\n",boot);fclose(boot);
            }
        }
        dense::shutdownGpuTelemetry();
        app=nullptr;
        return cancelled?1:0;
    }

    MSG message{};
    bool running=true;
    app->lastCameraUpdate=std::chrono::steady_clock::now();
    app->lastEnvironmentUpdate=app->lastCameraUpdate;
    app->pushHud(0.0f,16.0f);
    while(running) {
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT){running=false;break;}
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if(!running)break;
        const auto now=std::chrono::steady_clock::now();
        const float cameraDt=std::chrono::duration<float>(now-app->lastCameraUpdate).count();
        const float envDt=std::chrono::duration<float>(now-app->lastEnvironmentUpdate).count();
        app->lastCameraUpdate=now;
        app->lastEnvironmentUpdate=now;
        app->titleAge+=cameraDt;
        if(app->cameraMode==CameraMode::Cinematic)
            app->cinematicView(cameraDt);
        app->updateCamera(std::min(cameraDt,.10f));
        app->environment.update(envDt);
        const auto frameStart=std::chrono::steady_clock::now();
        app->renderer.render(app->cameraView(),app->debugSettings,
                             app->environment.constants(),dense::PlayerLocalLight{});
        const double frameMs=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-frameStart).count();
        app->smoothedMs=app->smoothedMs*.88+frameMs*.12;
        app->noteFrame(static_cast<float>(frameMs));
        const float fps=app->smoothedMs>0.05?static_cast<float>(1000.0/app->smoothedMs):0.0f;
        app->pushHud(fps,static_cast<float>(app->smoothedMs));
        if(benchFrames>0&&app->frameMsCount>=static_cast<std::size_t>(benchFrames)) {
            CreateDirectoryW(L"C:\\StressTest\\video",nullptr);
            std::ofstream out("C:\\StressTest\\video\\bench.txt");
            out<<"frames "<<app->frameMsCount<<"\n";
            out<<"avg_ms "<<app->smoothedMs<<"\n";
            out<<"avg_fps "<<fps<<"\n";
            out<<"p1_ms "<<app->percentileMs(.99f)<<"\n";
            out<<"max_ms "<<app->percentileMs(1.0f)<<"\n";
            out<<"temp_c "<<app->telemetry.temperatureC<<"\n";
            out<<"util "<<app->telemetry.utilizationPercent<<"\n";
            out<<"power_w "<<app->telemetry.powerWatts<<"\n";
            out<<"blades "<<app->bladeCount<<"\n";
            out<<"gpu_blades "<<app->renderer.pathTracedBladeCount()<<"\n";
            out<<"visible_near "<<app->renderer.visibleNearPatches()<<"\n";
            out<<"visible_far "<<app->renderer.visibleFarPatches()<<"\n";
            const auto&stream=app->renderer.streamStatus();
            out<<"dlss "<<stream.label<<"\n";
            out<<"render "<<stream.renderWidth<<"x"<<stream.renderHeight<<"\n";
            out<<"display "<<stream.displayWidth<<"x"<<stream.displayHeight<<"\n";
            out<<"mfg "<<stream.mfgMultiplier<<"\n";
            out<<"presented "<<stream.presentedFrames<<"\n";
            out<<"display_fps "<<fps*static_cast<float>(std::max<std::uint32_t>(1,stream.presentedFrames))<<"\n";
            PostQuitMessage(0);
            benchFrames=0;
        }
    }
    dense::shutdownGpuTelemetry();
    app=nullptr;
    return static_cast<int>(message.wParam);
}
