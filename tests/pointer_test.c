#include "rdk_client.h"

#include <freerdp/graphics.h>
#include <freerdp/pointer.h>
#include <freerdp/codec/color.h>
#include <stdio.h>
#include <string.h>

static freerdp* instance;
static rdpContext* context;
static rdkContext* rdk;
static HCURSOR originalCursor;
static rdpPointer* cachedPointers[4];

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s (Win32=%lu)\n", __func__, __LINE__, #expression, GetLastError()); \
	return FALSE; \
} } while (0)

static BOOL shape(UINT16 index, UINT32 bpp, UINT16 width, UINT16 height,
                  UINT16 hotX, UINT16 hotY, BYTE* xorData, UINT16 xorLength,
                  BYTE* andData, UINT16 andLength)
{
	rdpPointer* pointer = Pointer_Alloc(context);
	CHECK(pointer && index < ARRAYSIZE(cachedPointers));
	pointer->xorBpp = bpp;
	pointer->width = width;
	pointer->height = height;
	pointer->xPos = hotX;
	pointer->yPos = hotY;
	pointer->xorMaskData = xorData;
	pointer->lengthXorMask = xorLength;
	pointer->andMaskData = andData;
	pointer->lengthAndMask = andLength;
	if (!pointer->New(context, pointer))
	{
		pointer->Free(context, pointer);
		free(pointer);
		return FALSE;
	}
	if (cachedPointers[index])
	{
		cachedPointers[index]->Free(context, cachedPointers[index]);
		free(cachedPointers[index]);
	}
	cachedPointers[index] = pointer;
	return pointer->Set(context, pointer);
}

static BOOL render(UINT width, UINT height, UINT32 background, UINT32* output)
{
	BITMAPINFO bitmap = { 0 };
	bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmap.bmiHeader.biWidth = (LONG)width;
	bitmap.bmiHeader.biHeight = -(LONG)height;
	bitmap.bmiHeader.biPlanes = 1;
	bitmap.bmiHeader.biBitCount = 32;
	bitmap.bmiHeader.biCompression = BI_RGB;
	UINT32* pixels = NULL;
	HBITMAP image = CreateDIBSection(NULL, &bitmap, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
	CHECK(image && pixels);
	HDC dc = CreateCompatibleDC(NULL);
	CHECK(dc);
	HGDIOBJ previous = SelectObject(dc, image);
	for (UINT index = 0; index < width * height; ++index)
		pixels[index] = background;
	const BOOL drawn = DrawIconEx(dc, 0, 0, rdk->cursor, (int)width, (int)height, 0, NULL, DI_NORMAL);
	GdiFlush();
	memcpy(output, pixels, (size_t)width * height * sizeof(UINT32));
	SelectObject(dc, previous);
	DeleteDC(dc);
	DeleteObject(image);
	CHECK(drawn);
	return TRUE;
}

static BOOL cursor_info(UINT width, UINT height, UINT hotX, UINT hotY, BOOL monochrome)
{
	ICONINFO info = { 0 };
	CHECK(GetIconInfo(rdk->cursor, &info));
	BITMAP mask = { 0 };
	const BOOL obtained = GetObjectW(info.hbmMask, sizeof(mask), &mask) == sizeof(mask);
	const BOOL isMono = info.hbmColor == NULL;
	DeleteObject(info.hbmMask);
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	CHECK(obtained && !info.fIcon);
	CHECK(info.xHotspot == hotX && info.yHotspot == hotY);
	CHECK(isMono == monochrome);
	CHECK(mask.bmWidth == (LONG)width);
	CHECK(mask.bmHeight == (LONG)(monochrome ? height * 2 : height));
	return TRUE;
}

static BOOL monochrome_masks(void)
{
	BYTE andData[] = { 0xA0, 0, 0x50, 0 };
	BYTE xorData[] = { 0x60, 0, 0x90, 0 };
	CHECK(shape(0, 1, 9, 2, 3, 1, xorData, sizeof(xorData), andData, sizeof(andData)));
	CHECK(rdk->cursor && rdk->cursor != LoadCursorW(NULL, IDC_ARROW));
	CHECK(cursor_info(9, 2, 3, 1, TRUE));
	UINT32 pixels[18];
	CHECK(render(9, 2, 0x00345678, pixels));
	CHECK((pixels[0] & 0xFFFFFF) == 0x345678);
	CHECK((pixels[1] & 0xFFFFFF) == 0xFFFFFF);
	CHECK((pixels[2] & 0xFFFFFF) == (0x345678 ^ 0xFFFFFF));
	CHECK((pixels[3] & 0xFFFFFF) == 0);
	CHECK((pixels[9] & 0xFFFFFF) == 0xFFFFFF);
	CHECK((pixels[10] & 0xFFFFFF) == 0x345678);
	return TRUE;
}

static BOOL color_and_padding(void)
{
	BYTE andData[] = { 0x20, 0, 0, 0 };
	BYTE xorData[] = { 255, 255, 255, 0, 0, 0, 0, 0, 0, 0,
	                   0, 0, 255, 0, 255, 0, 255, 0, 0, 0 };
	CHECK(shape(1, 24, 3, 2, 2, 1, xorData, sizeof(xorData), andData, sizeof(andData)));
	CHECK(cursor_info(3, 2, 2, 1, FALSE));
	UINT32 pixels[6];
	CHECK(render(3, 2, 0x00345678, pixels));
	CHECK((pixels[0] & 0xFFFFFF) == 0xFF0000);
	CHECK((pixels[1] & 0xFFFFFF) == 0x00FF00);
	CHECK((pixels[2] & 0xFFFFFF) == 0x0000FF);
	CHECK((pixels[3] & 0xFFFFFF) == 0xFFFFFF);
	CHECK((pixels[4] & 0xFFFFFF) == 0);
	CHECK((pixels[5] & 0xFFFFFF) == 0x345678);
	return TRUE;
}

static BOOL alpha_cursor(void)
{
	BYTE xorData[] = { 0, 0, 255, 255, 0, 0, 0, 0 };
	CHECK(shape(2, 32, 2, 1, 1, 0, xorData, sizeof(xorData), NULL, 0));
	CHECK(cursor_info(2, 1, 1, 0, FALSE));
	UINT32 pixels[2];
	CHECK(render(2, 1, 0x00345678, pixels));
	CHECK((pixels[0] & 0xFFFFFF) == 0xFF0000);
	CHECK((pixels[1] & 0xFFFFFF) == 0x345678);
	ZeroMemory(xorData, sizeof(xorData));
	CHECK(shape(2, 32, 2, 1, 0, 0, xorData, sizeof(xorData), NULL, 0));
	CHECK(render(2, 1, 0x00345678, pixels));
	CHECK((pixels[0] & 0xFFFFFF) == 0x345678);
	CHECK((pixels[1] & 0xFFFFFF) == 0x345678);
	return TRUE;
}

static BOOL cached_and_system(void)
{
	CHECK(cachedPointers[0]->Set(context, cachedPointers[0]));
	HCURSOR first = rdk->cursor;
	CHECK(cachedPointers[1]->Set(context, cachedPointers[1]));
	CHECK(rdk->cursor != first);
	CHECK(context->graphics->Pointer_Prototype->SetNull(context));
	CHECK(rdk->cursor == NULL);
	rdk->hwnd = (HWND)(UINT_PTR)1;
	CHECK(rdk_pointer_setcursor(rdk, rdk->hwnd, HTCLIENT));
	CHECK(GetCursor() == NULL);
	SetCursor(originalCursor);
	CHECK(!rdk_pointer_setcursor(rdk, rdk->hwnd, HTCAPTION));
	CHECK(!rdk_pointer_setcursor(rdk, (HWND)(UINT_PTR)2, HTCLIENT));
	CHECK(context->graphics->Pointer_Prototype->SetDefault(context));
	CHECK(rdk->cursor == LoadCursorW(NULL, IDC_ARROW));
	CHECK(cachedPointers[0]->Set(context, cachedPointers[0]));
	CHECK(rdk->cursor == first);
	CHECK(rdk_pointer_setcursor(rdk, rdk->hwnd, HTCLIENT));
	CHECK(GetCursor() == first);
	SetCursor(originalCursor);
	rdk->hwnd = NULL;
	return TRUE;
}

static BOOL malformed_shapes(void)
{
	HCURSOR before = rdk->cursor;
	BYTE data[8] = { 0 };
	CHECK(!shape(3, 1, 9, 2, 0, 0, data, 1, data, 4));
	CHECK(!shape(3, 24, 3, 2, 0, 0, data, 1, data, 4));
	CHECK(!shape(3, 32, 0, 2, 0, 0, data, sizeof(data), NULL, 0));
	CHECK(!shape(3, 1, 9, 2, 9, 0, data, 4, data, 4));
	CHECK(!shape(3, 1, 385, 2, 0, 0, data, sizeof(data), data, sizeof(data)));
	CHECK(rdk->cursor == before);
	return TRUE;
}

static BOOL cache_replacement(void)
{
	BYTE andData[] = { 0x80, 0, 0x40, 0 };
	BYTE xorData[] = { 0x40, 0, 0x80, 0 };
	const DWORD initialGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
	const DWORD initialUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
	for (UINT index = 0; index < 100; ++index)
	{
		CHECK(shape(0, 1, 9, 2, (UINT16)(index % 9), 1,
		            xorData, sizeof(xorData), andData, sizeof(andData)));
		CHECK(cursor_info(9, 2, index % 9, 1, TRUE));
	}
	CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= initialGdi);
	CHECK(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= initialUser);
	return TRUE;
}

int main(void)
{
	originalCursor = GetCursor();
	instance = freerdp_new();
	if (!instance)
		return 1;
	instance->ContextSize = sizeof(rdkContext);
	if (!freerdp_context_new(instance))
	{
		freerdp_free(instance);
		return 1;
	}
	context = instance->context;
	rdk = (rdkContext*)context;
	BOOL ok = freerdp_settings_set_uint32(context->settings, FreeRDP_DesktopWidth, 32) &&
	          freerdp_settings_set_uint32(context->settings, FreeRDP_DesktopHeight, 32) &&
	          gdi_init(instance, PIXEL_FORMAT_BGRX32) && rdk_pointer_register(rdk);
	BOOL (*tests[])(void) = { monochrome_masks, color_and_padding, alpha_cursor,
	                         cached_and_system, malformed_shapes, cache_replacement };
	for (size_t index = 0; ok && index < ARRAYSIZE(tests); ++index)
		ok = tests[index]();
	SetCursor(originalCursor);
	rdk->hwnd = NULL;
	for (size_t index = 0; index < ARRAYSIZE(cachedPointers); ++index)
	{
		if (cachedPointers[index])
		{
			cachedPointers[index]->Free(context, cachedPointers[index]);
			free(cachedPointers[index]);
		}
	}
	gdi_free(instance);
	freerdp_context_free(instance);
	freerdp_free(instance);
	if (ok)
		printf("Passed %zu native pointer tests\n", ARRAYSIZE(tests));
	return ok ? 0 : 1;
}