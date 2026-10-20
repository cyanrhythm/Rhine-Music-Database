#include <QDebug>
#include <QtGlobal>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

using Microsoft::WRL::ComPtr;

void initializeWindowsGraphics() {
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) {
        qputenv("QSG_RHI_BACKEND", "d3d11");
    }
    if (qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX") ||
        qEnvironmentVariableIntValue("QSG_RHI_PREFER_SOFTWARE_RENDERER") != 0 ||
        qEnvironmentVariableIntValue("RHINE_SOFTWARE_GPU") == 1 || qgetenv("QSG_RHI_BACKEND") != "d3d11") {
        return;
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        qWarning() << "High-performance GPU enumeration unavailable; using Qt default adapter";
        return;
    }

    for (UINT preferenceIndex = 0;; ++preferenceIndex) {
        ComPtr<IDXGIAdapter1> preferredAdapter;
        const auto result =
            factory->EnumAdapterByGpuPreference(preferenceIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(preferredAdapter.GetAddressOf()));
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result)) {
            qWarning() << "High-performance GPU enumeration failed" << Qt::hex << result;
            break;
        }

        DXGI_ADAPTER_DESC1 preferredDescription{};
        if (FAILED(preferredAdapter->GetDesc1(&preferredDescription)) ||
            (preferredDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }

        ComPtr<ID3D11Device> probeDevice;
        if (FAILED(D3D11CreateDevice(preferredAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, probeDevice.GetAddressOf(), nullptr, nullptr))) {
            continue;
        }

        for (UINT adapterIndex = 0;; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf()))) {
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description))) {
                continue;
            }
            if (description.AdapterLuid.LowPart == preferredDescription.AdapterLuid.LowPart &&
                description.AdapterLuid.HighPart == preferredDescription.AdapterLuid.HighPart) {
                qputenv("QT_D3D_ADAPTER_INDEX", QByteArray::number(adapterIndex));
                qInfo() << "Selected high-performance D3D11 adapter" << adapterIndex
                        << QString::fromWCharArray(description.Description);
                return;
            }
        }
    }
    qWarning() << "No usable high-performance D3D11 adapter found; using Qt default adapter";
}
