#include "rdk_client.h"

#include <freerdp/graphics.h>
#include <freerdp/codec/color.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	rdpPointer common;
	HCURSOR cursor;
} rdkPointer;

static void rdk_pointer_apply(rdkContext* rdk)
{
	POINT position;
	if (rdk->hwnd && GetCursorPos(&position) &&
	    (WindowFromPoint(position) == rdk->hwnd || GetCapture() == rdk->hwnd))
		SetCursor(rdk->cursor);
}

static BOOL rdk_pointer_new(rdpContext* context, rdpPointer* pointer)
{
	if (!pointer || !context->gdi || pointer->width == 0 || pointer->height == 0 ||
	    pointer->width > 384 || pointer->height > 384 ||
	    pointer->xPos >= pointer->width || pointer->yPos >= pointer->height)
		return FALSE;
	ICONINFO info = { 0 };
	info.xHotspot = pointer->xPos;
	info.yHotspot = pointer->yPos;
	const size_t maskStride = ((pointer->width + 15) / 16) * 2;
	const size_t maskSize = maskStride * pointer->height;
	BYTE* mask = calloc(pointer->xorBpp == 1 ? 2 : 1, maskSize);
	if (!mask)
		return FALSE;
	BOOL ok = FALSE;
	if (pointer->xorBpp == 1)
	{
		if (!pointer->andMaskData || !pointer->xorMaskData ||
		    pointer->lengthAndMask < maskSize || pointer->lengthXorMask < maskSize)
			goto cleanup;
		memcpy(mask, pointer->andMaskData, maskSize);
		memcpy(mask + maskSize, pointer->xorMaskData, maskSize);
		info.hbmMask = CreateBitmap((int)pointer->width, (int)pointer->height * 2, 1, 1, mask);
	}
	else
	{
		BITMAPINFO bitmap = { 0 };
		bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmap.bmiHeader.biWidth = (LONG)pointer->width;
		bitmap.bmiHeader.biHeight = -(LONG)pointer->height;
		bitmap.bmiHeader.biPlanes = 1;
		bitmap.bmiHeader.biBitCount = 32;
		bitmap.bmiHeader.biCompression = BI_RGB;
		BYTE* pixels = NULL;
		info.hbmColor = CreateDIBSection(NULL, &bitmap, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
		if (!info.hbmColor || !pixels ||
		    !freerdp_image_copy_from_pointer_data(pixels, PIXEL_FORMAT_BGRA32, pointer->width * 4,
		        0, 0, pointer->width, pointer->height, pointer->xorMaskData, pointer->lengthXorMask,
		        pointer->andMaskData, pointer->lengthAndMask, pointer->xorBpp, &context->gdi->palette))
			goto cleanup;
		for (UINT32 row = 0; row < pointer->height; ++row)
		{
			for (UINT32 column = 0; column < pointer->width; ++column)
			{
				if (pixels[((size_t)row * pointer->width + column) * 4 + 3] == 0)
					mask[row * maskStride + column / 8] |= (BYTE)(0x80 >> (column % 8));
			}
		}
		info.hbmMask = CreateBitmap((int)pointer->width, (int)pointer->height, 1, 1, mask);
	}
	if (!info.hbmMask)
		goto cleanup;
	((rdkPointer*)pointer)->cursor = (HCURSOR)CreateIconIndirect(&info);
	ok = ((rdkPointer*)pointer)->cursor != NULL;
cleanup:
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	if (info.hbmMask)
		DeleteObject(info.hbmMask);
	free(mask);
	return ok;
}

static BOOL rdk_pointer_set_default(rdpContext* context)
{
	rdkContext* rdk = (rdkContext*)context;
	rdk->cursor = LoadCursorW(NULL, IDC_ARROW);
	rdk_pointer_apply(rdk);
	return rdk->cursor != NULL;
}

static void rdk_pointer_free(rdpContext* context, rdpPointer* pointer)
{
	rdkPointer* native = (rdkPointer*)pointer;
	if (!native || !native->cursor)
		return;
	rdkContext* rdk = (rdkContext*)context;
	if (rdk->cursor == native->cursor)
		(void)rdk_pointer_set_default(context);
	if (GetCursor() == native->cursor)
		SetCursor(LoadCursorW(NULL, IDC_ARROW));
	DestroyCursor(native->cursor);
	native->cursor = NULL;
}

static BOOL rdk_pointer_set(rdpContext* context, rdpPointer* pointer)
{
	rdkContext* rdk = (rdkContext*)context;
	if (!pointer || !((rdkPointer*)pointer)->cursor)
		return FALSE;
	rdk->cursor = ((rdkPointer*)pointer)->cursor;
	rdk_pointer_apply(rdk);
	return TRUE;
}

static BOOL rdk_pointer_set_null(rdpContext* context)
{
	rdkContext* rdk = (rdkContext*)context;
	rdk->cursor = NULL;
	rdk_pointer_apply(rdk);
	return TRUE;
}

BOOL rdk_pointer_register(rdkContext* rdk)
{
	rdpContext* context = (rdpContext*)rdk;
	rdpPointer pointer = { 0 };
	pointer.size = sizeof(rdkPointer);
	pointer.New = rdk_pointer_new;
	pointer.Free = rdk_pointer_free;
	pointer.Set = rdk_pointer_set;
	pointer.SetNull = rdk_pointer_set_null;
	pointer.SetDefault = rdk_pointer_set_default;
	graphics_register_pointer(context->graphics, &pointer);
	return rdk_pointer_set_default(context);
}

BOOL rdk_pointer_setcursor(rdkContext* rdk, HWND window, UINT hitTest)
{
	if (!rdk || window != rdk->hwnd || hitTest != HTCLIENT)
		return FALSE;
	SetCursor(rdk->cursor);
	return TRUE;
}