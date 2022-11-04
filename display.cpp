#include <glib.h>
#include <spice.h>
#include <memory>
#include <cassert>

#include "IDXGIOutputDuplication/DuplicationManager.h"
#include "IDXGIOutputDuplication/PixelShader.h"
#include "IDXGIOutputDuplication/VertexShader.h"

#include <cstdio>

#include "display.h"

struct asset {
	DX_RESOURCES *rsrc;
	ID3D11Texture2D *texture;
	D3D11_MAPPED_SUBRESOURCE *mapInfo;
};

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

static QXLCursorCmd *create_cursor_move_cmd(POINT position)
{
	QXLCursorCmd *cmd;

	cmd = (QXLCursorCmd *)calloc(1, sizeof(*cmd));
	if (!cmd)
		return NULL;

	cmd->release_info.id = (uintptr_t)cmd;
	cmd->type = QXL_CURSOR_MOVE;
	cmd->u.position.x = position.x;
	cmd->u.position.y = position.y;

	cmd->release_info.id = (uintptr_t)cmd;

	return cmd;
}

static QXLCursorCmd *create_cursor_set_cmd(PTR_INFO *ptr_info)
{
	QXLCursorCmd *cmd;
	QXLCursor *cursor;
	unsigned int height_factor = 1;

	cmd = (QXLCursorCmd*)calloc(1, sizeof(*cmd) + sizeof(*cursor) + ptr_info->BufferSize);
	if (!cmd)
		return NULL;

	cursor = (QXLCursor *) (cmd + 1);

	cursor->header.unique = 0;
	switch(ptr_info->ShapeInfo.Type) {
	case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
		cursor->header.type = SPICE_CURSOR_TYPE_MONO;
		/*
		 * height value is twice as big as expected, probably because pixels are present
		 * two times: once as AND mask, once as XOR-mask
		 */
		height_factor = 2;
		break;
	case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
		cursor->header.type = SPICE_CURSOR_TYPE_ALPHA;
		break;
	default:
	case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
		printf("ShapeInfo.Type %d not implemented, try SPICE_CURSOR_TYPE_ALPHA\n", ptr_info->ShapeInfo.Type);
		cursor->header.type = SPICE_CURSOR_TYPE_ALPHA;
		break;
	};

	cursor->header.width = ptr_info->ShapeInfo.Width;
	cursor->header.height = ptr_info->ShapeInfo.Height / height_factor;

	cursor->header.hot_spot_x = ptr_info->ShapeInfo.HotSpot.x;
	cursor->header.hot_spot_y = ptr_info->ShapeInfo.HotSpot.y;

	cursor->data_size = ptr_info->BufferSize;

	cursor->chunk.next_chunk = 0;
	cursor->chunk.prev_chunk = 0;
	cursor->chunk.data_size = ptr_info->BufferSize;

	memcpy(cursor->chunk.data, ptr_info->PtrShapeBuffer, ptr_info->BufferSize);

	cmd->type = QXL_CURSOR_SET;
	cmd->u.set.position.x = ptr_info->Position.x;
	cmd->u.set.position.y = ptr_info->Position.y;
	cmd->u.set.shape = (uintptr_t) cursor;
	cmd->u.set.visible = TRUE;

	cmd->release_info.id = (uintptr_t)cmd;

	return cmd;
}

void ProcessPointer(PTR_INFO *ptr_info, GAsyncQueue *cursor_queue)
{
	/*
	 * A zero value indicates that the position or shape of the mouse was not
	 * updated since an application last called the
	 * IDXGIOutputDuplication::AcquireNextFrame method to acquire the next frame
	 * of the desktop image.
	 */
	if (ptr_info->LastTimeStamp.QuadPart) {
		QXLCursorCmd *cursor_info = create_cursor_move_cmd(ptr_info->Position);
		g_async_queue_push(cursor_queue, cursor_info);
	}

	/*
	 * A new pointer shape is indicated by a non-zero value in the
	 * PointerShapeBufferSize member.
	 */
	if (ptr_info->BufferSize) {
		QXLCursorCmd *cursor_info = create_cursor_set_cmd(ptr_info);
		g_async_queue_push(cursor_queue, cursor_info);
	}
}

static QXLDrawable *create_drawable(int x, int y, int w, int h, int stride, void *pixels, void *asset)
{
	QXLDrawable *drawable;
	QXLImage *qxl_image;
	int i;

	drawable = (QXLDrawable *)calloc(1, sizeof(*drawable) + sizeof(*qxl_image));
	if (!drawable)
		return NULL;
	qxl_image = (QXLImage *) (drawable + 1);

	drawable->release_info.id = (uintptr_t)asset;

	drawable->surface_id = 0;
	drawable->type = QXL_DRAW_COPY;
	drawable->effect = QXL_EFFECT_OPAQUE;
	drawable->clip.type = SPICE_CLIP_TYPE_NONE;
	drawable->bbox.left = x;
	drawable->bbox.top = y;
	drawable->bbox.right = x + w - 1;
	drawable->bbox.bottom = y + h - 1;

	for (i = 0; i < 3; ++i)
		drawable->surfaces_dest[i] = -1;

	drawable->u.copy.src_area.left = 0;
	drawable->u.copy.src_area.top = 0;
	drawable->u.copy.src_area.right = w;
	drawable->u.copy.src_area.bottom = h;
	drawable->u.copy.rop_descriptor = SPICE_ROPD_OP_PUT;

	drawable->u.copy.src_bitmap = (uintptr_t) qxl_image;

	qxl_image->descriptor.id = 0;
	qxl_image->descriptor.type = SPICE_IMAGE_TYPE_BITMAP;

	qxl_image->descriptor.flags = 0;
	qxl_image->descriptor.width = w;
	qxl_image->descriptor.height = h;

	qxl_image->bitmap.format = SPICE_BITMAP_FMT_RGBA;
	qxl_image->bitmap.flags = SPICE_BITMAP_FLAGS_TOP_DOWN | QXL_BITMAP_DIRECT;
	qxl_image->bitmap.x = w;
	qxl_image->bitmap.y = h;
	qxl_image->bitmap.stride = stride;
	qxl_image->bitmap.palette = 0;
	qxl_image->bitmap.data = (uintptr_t)pixels;

	return drawable;
}

void ProcessFrame(DX_RESOURCES *rsrc, FRAME_DATA *current_data, GAsyncQueue *draw_queue, QXLInstance *display_sin)
{
	HRESULT hr;
//	printf("Dirty %u, Moved %u\n", current_data->DirtyCount, current_data->MoveCount);
	DXGI_OUTDUPL_MOVE_RECT *pMoveRect = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(current_data->MetaData);
	RECT *pDirtyRect = reinterpret_cast<RECT*>(current_data->MetaData + (current_data->MoveCount * sizeof(DXGI_OUTDUPL_MOVE_RECT)));

	D3D11_TEXTURE2D_DESC desc;
	current_data->Frame->GetDesc(&desc);

	assert(current_data->MoveCount == 0);

//	printf("Dirty:\n");
	for (unsigned int k = 0; k < current_data->DirtyCount; ++k)
	{
		unsigned int w = pDirtyRect->right - pDirtyRect->left;
		unsigned int h = pDirtyRect->bottom - pDirtyRect->top;
		unsigned int depth = 4;

#if 0
		printf("  left %ld, top %ld, right %ld, bottom %ld, w %u, h %u\n", 
			pDirtyRect->left,
			pDirtyRect->top,
			pDirtyRect->right,
			pDirtyRect->bottom,
			w,
			h);
#endif

		D3D11_TEXTURE2D_DESC desc2;
		desc2.Width = w;
		desc2.Height = h;
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

		rsrc->Context->CopySubresourceRegion(stagingTexture, 0, 0, 0, 0, current_data->Frame, 0, &sourceRegion);

		D3D11_MAPPED_SUBRESOURCE *mapInfo = new D3D11_MAPPED_SUBRESOURCE;
		hr = rsrc->Context->Map(
				stagingTexture,
				0,
				D3D11_MAP_READ,
				0,
				mapInfo);
		if (FAILED(hr)) {
			printf("Failed to map staging texture\n");
		}

		void *buf = malloc(h * mapInfo->RowPitch);
		memcpy(buf, mapInfo->pData, h * mapInfo->RowPitch);

		QXLDrawable *drawable = create_drawable(
			pDirtyRect->left,
			pDirtyRect->top,
			w,
			h,
			mapInfo->RowPitch,
			buf,
			buf);

		rsrc->Context->Unmap(stagingTexture, 0);

		stagingTexture->Release();

		g_async_queue_push(draw_queue, drawable);

		spice_qxl_wakeup(display_sin);

		pDirtyRect++;
	}
}

void release_asset(void *data)
{
	free(data);
}

gpointer display(gpointer data)
{
	struct display_config *cfg = reinterpret_cast<struct display_config*>(data);
	DUPLICATIONMANAGER mgr;
	DUPL_RETURN ret;
	DX_RESOURCES rsrc;

	ret = InitializeDx(&rsrc);
	if (ret != DUPL_RETURN_SUCCESS)
	{
		fprintf(stderr, "InitializeDx returned %d\n", ret);
		exit(EXIT_FAILURE);
	}

	ret = mgr.InitDupl(rsrc.Device, 0);
	if (ret != DUPL_RETURN_SUCCESS)
	{
		fprintf(stderr, "InitDupl returned %d\n", ret);
		exit(EXIT_FAILURE);
	}

	bool quit = false;
	FRAME_DATA current_data;

	while( quit == false ) {
		bool TimeOut;
		ret = mgr.GetFrame(&current_data, &TimeOut);
		if (ret != DUPL_RETURN_SUCCESS)
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

		ProcessFrame(&rsrc, &current_data, cfg->draw_queue, cfg->display_sin);

		PTR_INFO ptr_info;
		memset(&ptr_info, 0, sizeof(ptr_info));

		ret = mgr.GetMouse(&ptr_info, &current_data.FrameInfo, 0, 0);
		if (ret == DUPL_RETURN_SUCCESS)
			ProcessPointer(&ptr_info, cfg->cursor_queue);

		mgr.DoneWithFrame();

		free(ptr_info.PtrShapeBuffer);
	}

	return 0;
}
