#include <memory>

#include <SDL.h>

#include "DuplicationManager.h"
#include "PixelShader.h"
#include "VertexShader.h"

const int SCREEN_WIDTH = 640;
const int SCREEN_HEIGHT = 480;

DUPL_RETURN InitializeDx(_Out_ DX_RESOURCES* Data)
{
    HRESULT hr = S_OK;

    // Driver types supported
    D3D_DRIVER_TYPE DriverTypes[] =
    {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

    // Feature levels supported
    D3D_FEATURE_LEVEL FeatureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1
    };
    UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

    D3D_FEATURE_LEVEL FeatureLevel;

    // Create device
    for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex)
    {
        hr = D3D11CreateDevice(nullptr, DriverTypes[DriverTypeIndex], nullptr, 0, FeatureLevels, NumFeatureLevels,
                                D3D11_SDK_VERSION, &Data->Device, &FeatureLevel, &Data->Context);
        if (SUCCEEDED(hr))
        {
            // Device creation success, no need to loop anymore
            break;
        }
    }
    if (FAILED(hr))
    {
        return ProcessFailure(nullptr, L"Failed to create device in InitializeDx", L"Error", hr);
    }

    // VERTEX shader
    UINT Size = ARRAYSIZE(g_VS);
    hr = Data->Device->CreateVertexShader(g_VS, Size, nullptr, &Data->VertexShader);
    if (FAILED(hr))
    {
        return ProcessFailure(Data->Device, L"Failed to create vertex shader in InitializeDx", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Input layout
    D3D11_INPUT_ELEMENT_DESC Layout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    UINT NumElements = ARRAYSIZE(Layout);
    hr = Data->Device->CreateInputLayout(Layout, NumElements, g_VS, Size, &Data->InputLayout);
    if (FAILED(hr))
    {
        return ProcessFailure(Data->Device, L"Failed to create input layout in InitializeDx", L"Error", hr, SystemTransitionsExpectedErrors);
    }
    Data->Context->IASetInputLayout(Data->InputLayout);

    // Pixel shader
    Size = ARRAYSIZE(g_PS);
    hr = Data->Device->CreatePixelShader(g_PS, Size, nullptr, &Data->PixelShader);
    if (FAILED(hr))
    {
        return ProcessFailure(Data->Device, L"Failed to create pixel shader in InitializeDx", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Set up sampler
    D3D11_SAMPLER_DESC SampDesc;
    RtlZeroMemory(&SampDesc, sizeof(SampDesc));
    SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    SampDesc.MinLOD = 0;
    SampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = Data->Device->CreateSamplerState(&SampDesc, &Data->SamplerLinear);
    if (FAILED(hr))
    {
        return ProcessFailure(Data->Device, L"Failed to create sampler state in InitializeDx", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    return DUPL_RETURN_SUCCESS;
}

void ProcessFrame(DX_RESOURCES *rsrc, FRAME_DATA *CurrentData, void *pixels)
{
    HRESULT hr;
    printf("Dirty %u, Moved %u\n", CurrentData->DirtyCount, CurrentData->MoveCount);
    DXGI_OUTDUPL_MOVE_RECT *pMoveRect = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(CurrentData->MetaData);
    RECT *pDirtyRect = reinterpret_cast<RECT*>(CurrentData->MetaData + (CurrentData->MoveCount * sizeof(DXGI_OUTDUPL_MOVE_RECT)));

    D3D11_TEXTURE2D_DESC desc;
    CurrentData->Frame->GetDesc(&desc);

    printf("Dirty:\n");
    for (unsigned int k = 0; k < CurrentData->DirtyCount; ++k)
    {
        printf("  left %ld, top %ld, right %ld, bottom %ld\n", 
            k,
            pDirtyRect->left,
            pDirtyRect->top,
            pDirtyRect->right,
            pDirtyRect->bottom);

        D3D11_TEXTURE2D_DESC desc2;
        desc2.Width = SCREEN_WIDTH;
        desc2.Height = SCREEN_HEIGHT;
        desc2.MipLevels = desc.MipLevels;
        desc2.ArraySize = desc.ArraySize;
        desc2.Format = desc.Format;
        desc2.SampleDesc = desc.SampleDesc;
        desc2.Usage = D3D11_USAGE_STAGING;
        desc2.BindFlags = 0;
        desc2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc2.MiscFlags = 0;

        ID3D11Texture2D *stagingTexture;
        hr = rsrc->Device->CreateTexture2D(&desc2, nullptr, &stagingTexture);
        if (FAILED(hr)) {
            printf("Failed to create staging texture\n");
        }

        D3D11_BOX sourceRegion;
        sourceRegion.left = pDirtyRect->left;
        sourceRegion.right = pDirtyRect->right;
        sourceRegion.top = pDirtyRect->top;
        sourceRegion.bottom = pDirtyRect->bottom;
        sourceRegion.front = 0;
        sourceRegion.back = 1;

        rsrc->Context->CopySubresourceRegion(stagingTexture, 0, 0, 0, 0, CurrentData->Frame, 0, &sourceRegion);

        D3D11_MAPPED_SUBRESOURCE mapInfo;
        hr = rsrc->Context->Map(
                stagingTexture,
                0,
                D3D11_MAP_READ,
                0,
                &mapInfo);
        if (FAILED(hr)) {
            printf("Failed to map staging texture\n");
        }

        memcpy(pixels, mapInfo.pData, 640*480*4);

        rsrc->Context->Unmap(stagingTexture, 0);

        stagingTexture->Release();
    }
}

int main( int argc, char* args[] )
{
    DUPLICATIONMANAGER mgr;
    DUPL_RETURN Ret;
    DX_RESOURCES rsrc;

    Ret = InitializeDx(&rsrc);
    if (Ret != DUPL_RETURN_SUCCESS)
    {
        fprintf(stderr, "InitializeDx returned %d\n", Ret);
        exit(EXIT_FAILURE);
    }

    Ret = mgr.InitDupl(rsrc.Device, 0);
    if (Ret != DUPL_RETURN_SUCCESS)
    {
        fprintf(stderr, "InitDupl returned %d\n", Ret);
        exit(EXIT_FAILURE);
    }

    //The window we'll be rendering to
    SDL_Window* window = NULL;
    
    //The surface contained by the window
    SDL_Surface* screenSurface = NULL;

    //Initialize SDL
    if( SDL_Init( SDL_INIT_VIDEO ) < 0 )
    {
        printf( "SDL could not initialize! SDL_Error: %s\n", SDL_GetError() );
    }
    else
    {
        //Create window
        window = SDL_CreateWindow( "SDL Tutorial", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN );
        if( window == NULL )
        {
            printf( "Window could not be created! SDL_Error: %s\n", SDL_GetError() );
        }
        else
        {
            //Get window surface
            screenSurface = SDL_GetWindowSurface( window );

            //Fill the surface white
            SDL_FillRect( screenSurface, NULL, SDL_MapRGB( screenSurface->format, 0xFF, 0xFF, 0xFF ) );
            
            //Update the surface
            SDL_UpdateWindowSurface( window );
            
            //Hack to get window to stay up
            SDL_Event e;
            bool quit = false;
            FRAME_DATA CurrentData;

            while( quit == false ) {
                while( SDL_PollEvent( &e ) ){
                    if( e.type == SDL_QUIT )
                        quit = true; 
                }

                bool TimeOut;
                Ret = mgr.GetFrame(&CurrentData, &TimeOut);
                if (Ret != DUPL_RETURN_SUCCESS)
                {
                    // An error occurred getting the next frame drop out of loop which
                    // will check if it was expected or not
                    break;
                }

                // Check for timeout
                if (TimeOut)
                {
                    // No new frame at the moment
                    continue;
                }

                ProcessFrame(&rsrc, &CurrentData, screenSurface->pixels);

                mgr.DoneWithFrame();

                SDL_UpdateWindowSurface( window );

            }
        }
    }

    //Destroy window
    SDL_DestroyWindow( window );

    //Quit SDL subsystems
    SDL_Quit();

    return 0;
}
