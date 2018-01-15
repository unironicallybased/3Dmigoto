#include "HackerContext.h"
#include "Globals.h"

#include <ScreenGrab.h>
#include <wincodec.h>
#include <Strsafe.h>
#include <stdarg.h>

FrameAnalysisContext::FrameAnalysisContext(ID3D11DeviceContext1 *pContext)
{
	mOrigContext = pContext;

	frame_analysis_log = NULL;
}

FrameAnalysisContext::~FrameAnalysisContext()
{
	if (frame_analysis_log)
		fclose(frame_analysis_log);
}

void FrameAnalysisContext::SetContext(ID3D11DeviceContext1 *pContext)
{
	mOrigContext = pContext;
}

void FrameAnalysisContext::vFrameAnalysisLog(char *fmt, va_list ap)
{
	wchar_t filename[MAX_PATH];

	LogDebugNoNL("FrameAnalysisContext(%s@%p)::", type_name(this), this);
	vLogDebug(fmt, ap);

	if (!G->analyse_frame) {
		if (frame_analysis_log)
			fclose(frame_analysis_log);
		frame_analysis_log = NULL;
		return;
	}

	// DSS note: the below comment was originally referring to the
	// C->cur_analyse_options & FrameAnalysisOptions::LOG test we used to
	// have here, but even though we removed that test this is still a good
	// reminder for other settings as well.
	//
	// Using the global analyse options here as the local copy in the
	// context is only updated after draw calls. We could potentially
	// process the triggers here, but this function is intended to log
	// other calls as well where that wouldn't make sense. We could change
	// it so that this is called from FrameAnalysisAfterDraw, but we want
	// to log calls for deferred contexts here as well.

	if (!frame_analysis_log) {
		// Use the original context to check the type, otherwise we
		// will recursively call ourselves:
		if (mOrigContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
			swprintf_s(filename, MAX_PATH, L"%ls\\log.txt", G->ANALYSIS_PATH);
		else
			swprintf_s(filename, MAX_PATH, L"%ls\\log-0x%p.txt", G->ANALYSIS_PATH, this);

		frame_analysis_log = _wfsopen(filename, L"w", _SH_DENYNO);
		if (!frame_analysis_log) {
			LogInfoW(L"Error opening %s\n", filename);
			return;
		}

		fprintf(frame_analysis_log, "analyse_options: %08x\n", G->cur_analyse_options);
	}

	// We don't allow hold to be changed mid-frame due to potential
	// for filename conflicts, so use def_analyse_options:
	if (G->def_analyse_options & FrameAnalysisOptions::HOLD)
		fprintf(frame_analysis_log, "%u.", G->analyse_frame_no);
	fprintf(frame_analysis_log, "%06u ", G->analyse_frame);

	vfprintf(frame_analysis_log, fmt, ap);
}

void FrameAnalysisContext::FrameAnalysisLog(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vFrameAnalysisLog(fmt, ap);
	va_end(ap);
}

void HackerContext::FrameAnalysisLog(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	FAContext.vFrameAnalysisLog(fmt, ap);
	va_end(ap);
}

#define FALogInfo(fmt, ...) { \
	LogInfo("Frame Analysis: " fmt, __VA_ARGS__); \
	FrameAnalysisLog(fmt, __VA_ARGS__); \
} while (0)


static void FrameAnalysisLogSlot(FILE *frame_analysis_log, int slot, char *slot_name)
{
	if (slot_name)
		fprintf(frame_analysis_log, "       %s:", slot_name);
	else if (slot != -1)
		fprintf(frame_analysis_log, "       %u:", slot);
}

template <class ID3D11Shader>
void FrameAnalysisContext::FrameAnalysisLogShaderHash(ID3D11Shader *shader)
{
	UINT64 hash;

	// Always complete the line in the debug log:
	LogDebug("\n");

	if (!G->analyse_frame || !frame_analysis_log)
		return;

	if (!shader) {
		fprintf(frame_analysis_log, "\n");
		return;
	}

	EnterCriticalSection(&G->mCriticalSection);

	try {
		hash = G->mShaders.at(shader);
		if (hash)
			fprintf(frame_analysis_log, " hash=%016llx", hash);
	} catch (std::out_of_range) {
	}

	LeaveCriticalSection(&G->mCriticalSection);

	fprintf(frame_analysis_log, "\n");
}

void FrameAnalysisContext::FrameAnalysisLogResourceHash(ID3D11Resource *resource)
{
	uint32_t hash, orig_hash;
	struct ResourceHashInfo *info;

	// Always complete the line in the debug log:
	LogDebug("\n");

	if (!G->analyse_frame || !frame_analysis_log)
		return;

	if (!resource) {
		fprintf(frame_analysis_log, "\n");
		return;
	}

	EnterCriticalSection(&G->mCriticalSection);

	try {
		hash = G->mResources.at(resource).hash;
		orig_hash = G->mResources.at(resource).orig_hash;
		if (hash)
			fprintf(frame_analysis_log, " hash=%08x", hash);
		if (orig_hash != hash)
			fprintf(frame_analysis_log, " orig_hash=%08x", orig_hash);

		info = &G->mResourceInfo.at(orig_hash);
		if (info->hash_contaminated) {
			fprintf(frame_analysis_log, " hash_contamination=");
			if (!info->map_contamination.empty())
				fprintf(frame_analysis_log, "Map,");
			if (!info->update_contamination.empty())
				fprintf(frame_analysis_log, "UpdateSubresource,");
			if (!info->copy_contamination.empty())
				fprintf(frame_analysis_log, "CopyResource,");
			if (!info->region_contamination.empty())
				fprintf(frame_analysis_log, "UpdateSubresourceRegion,");
		}
	} catch (std::out_of_range) {
	}

	LeaveCriticalSection(&G->mCriticalSection);

	fprintf(frame_analysis_log, "\n");
}

void FrameAnalysisContext::FrameAnalysisLogResource(int slot, char *slot_name, ID3D11Resource *resource)
{
	if (!resource || !G->analyse_frame || !frame_analysis_log)
		return;

	FrameAnalysisLogSlot(frame_analysis_log, slot, slot_name);
	fprintf(frame_analysis_log, " resource=0x%p", resource);

	FrameAnalysisLogResourceHash(resource);
}

void FrameAnalysisContext::FrameAnalysisLogView(int slot, char *slot_name, ID3D11View *view)
{
	ID3D11Resource *resource;

	if (!view || !G->analyse_frame || !frame_analysis_log)
		return;

	FrameAnalysisLogSlot(frame_analysis_log, slot, slot_name);
	fprintf(frame_analysis_log, " view=0x%p", view);

	view->GetResource(&resource);
	if (!resource)
		return;

	FrameAnalysisLogResource(-1, NULL, resource);

	resource->Release();
}

void FrameAnalysisContext::FrameAnalysisLogResourceArray(UINT start, UINT len, ID3D11Resource *const *ppResources)
{
	UINT i;

	if (!ppResources || !G->analyse_frame || !frame_analysis_log)
		return;

	for (i = 0; i < len; i++)
		FrameAnalysisLogResource(start + i, NULL, ppResources[i]);
}

void FrameAnalysisContext::FrameAnalysisLogViewArray(UINT start, UINT len, ID3D11View *const *ppViews)
{
	UINT i;

	if (!ppViews || !G->analyse_frame || !frame_analysis_log)
		return;

	for (i = 0; i < len; i++)
		FrameAnalysisLogView(start + i, NULL, ppViews[i]);
}

void FrameAnalysisContext::FrameAnalysisLogMiscArray(UINT start, UINT len, void *const *array)
{
	UINT i;
	void *item;

	if (!array || !G->analyse_frame || !frame_analysis_log)
		return;

	for (i = 0; i < len; i++) {
		item = array[i];
		if (item) {
			FrameAnalysisLogSlot(frame_analysis_log, start + i, NULL);
			fprintf(frame_analysis_log, " handle=0x%p\n", item);
		}
	}
}

static void FrameAnalysisLogQuery(ID3D11Query *query)
{
	D3D11_QUERY_DESC desc;

	query->GetDesc(&desc);
}

void FrameAnalysisContext::FrameAnalysisLogAsyncQuery(ID3D11Asynchronous *async)
{
	AsyncQueryType type;
	ID3D11Query *query;
	ID3D11Predicate *predicate;
	D3D11_QUERY_DESC desc;

	// Always complete the line in the debug log:
	LogDebug("\n");

	if (!G->analyse_frame || !frame_analysis_log)
		return;

	if (!async) {
		fprintf(frame_analysis_log, "\n");
		return;
	}

	try {
		type = G->mQueryTypes.at(async);
	} catch (std::out_of_range) {
		return;
	}

	switch (type) {
		case AsyncQueryType::QUERY:
			fprintf(frame_analysis_log, " type=query query=");
			query = (ID3D11Query*)async;
			query->GetDesc(&desc);
			break;
		case AsyncQueryType::PREDICATE:
			fprintf(frame_analysis_log, " type=predicate query=");
			predicate = (ID3D11Predicate*)async;
			predicate->GetDesc(&desc);
			break;
		case AsyncQueryType::COUNTER:
			fprintf(frame_analysis_log, " type=performance\n");
			// Don't care about this, and it's a different DESC
			return;
		default:
			// Should not happen
			return;
	}

	switch (desc.Query) {
		case D3D11_QUERY_EVENT:
			fprintf(frame_analysis_log, "event");
			break;
		case D3D11_QUERY_OCCLUSION:
			fprintf(frame_analysis_log, "occlusion");
			break;
		case D3D11_QUERY_TIMESTAMP:
			fprintf(frame_analysis_log, "timestamp");
			break;
		case D3D11_QUERY_TIMESTAMP_DISJOINT:
			fprintf(frame_analysis_log, "timestamp_disjoint");
			break;
		case D3D11_QUERY_PIPELINE_STATISTICS:
			fprintf(frame_analysis_log, "pipeline_statistics");
			break;
		case D3D11_QUERY_OCCLUSION_PREDICATE:
			fprintf(frame_analysis_log, "occlusion_predicate");
			break;
		case D3D11_QUERY_SO_STATISTICS:
			fprintf(frame_analysis_log, "so_statistics");
			break;
		case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
			fprintf(frame_analysis_log, "so_overflow_predicate");
			break;
		case D3D11_QUERY_SO_STATISTICS_STREAM0:
			fprintf(frame_analysis_log, "so_statistics_stream0");
			break;
		case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
			fprintf(frame_analysis_log, "so_overflow_predicate_stream0");
			break;
		case D3D11_QUERY_SO_STATISTICS_STREAM1:
			fprintf(frame_analysis_log, "so_statistics_stream1");
			break;
		case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
			fprintf(frame_analysis_log, "so_overflow_predicate_stream1");
			break;
		case D3D11_QUERY_SO_STATISTICS_STREAM2:
			fprintf(frame_analysis_log, "so_statistics_stream2");
			break;
		case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
			fprintf(frame_analysis_log, "so_overflow_predicate_stream2");
			break;
		case D3D11_QUERY_SO_STATISTICS_STREAM3:
			fprintf(frame_analysis_log, "so_statistics_stream3");
			break;
		case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
			fprintf(frame_analysis_log, "so_overflow_predicate_stream3");
			break;
		default:
			fprintf(frame_analysis_log, "?");
			break;
	}
	fprintf(frame_analysis_log, " MiscFlags=0x%x\n", desc.MiscFlags);
}

void FrameAnalysisContext::FrameAnalysisLogData(void *buf, UINT size)
{
	unsigned char *ptr = (unsigned char*)buf;
	UINT i;

	if (!buf || !size || !G->analyse_frame || !frame_analysis_log)
		return;

	fprintf(frame_analysis_log, "    data: ");
	for (i = 0; i < size; i++, ptr++)
		fprintf(frame_analysis_log, "%02x", *ptr);
	fprintf(frame_analysis_log, "\n");
}

void HackerContext::Dump2DResource(ID3D11Texture2D *resource, wchar_t
		*filename, bool stereo, FrameAnalysisOptions type_mask)
{
	FrameAnalysisOptions options = (FrameAnalysisOptions)(analyse_options & type_mask);
	HRESULT hr = S_OK, dont_care;
	wchar_t *ext;

	ext = wcsrchr(filename, L'.');
	if (!ext) {
		FALogInfo("Dump2DResource: Filename missing extension\n");
		return;
	}

	// Needs to be called at some point before SaveXXXTextureToFile:
	dont_care = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	if ((options & FrameAnalysisOptions::DUMP_XXX_JPS) ||
	    (options & FrameAnalysisOptions::DUMP_XXX)) {
		// save a JPS file. This will be missing extra channels (e.g.
		// transparency, depth buffer, specular power, etc) or bit depth that
		// can be found in the DDS file, but is generally easier to work with.
		//
		// Not all formats can be saved as JPS with this function - if
		// only dump_rt was specified (as opposed to dump_rt_jps) we
		// will dump out DDS files for those instead.
		if (stereo)
			wcscpy_s(ext, MAX_PATH + filename - ext, L".jps");
		else
			wcscpy_s(ext, MAX_PATH + filename - ext, L".jpg");
		hr = DirectX::SaveWICTextureToFile(mPassThroughContext1, resource, GUID_ContainerFormatJpeg, filename);
	}


	if ((options & FrameAnalysisOptions::DUMP_XXX_DDS) ||
	   ((options & FrameAnalysisOptions::DUMP_XXX) && FAILED(hr))) {
		wcscpy_s(ext, MAX_PATH + filename - ext, L".dds");
		hr = DirectX::SaveDDSTextureToFile(mPassThroughContext1, resource, filename);
	}

	if (FAILED(hr))
		FALogInfo("Failed to dump Texture2D: 0x%x\n", hr);
}

HRESULT HackerContext::CreateStagingResource(ID3D11Texture2D **resource,
		D3D11_TEXTURE2D_DESC desc, bool stereo, bool msaa)
{
	NVAPI_STEREO_SURFACECREATEMODE orig_mode = NVAPI_STEREO_SURFACECREATEMODE_AUTO;
	HRESULT hr;

	// NOTE: desc is passed by value - this is intentional so we don't
	// modify desc in the caller

	if (stereo)
		desc.Width *= 2;

	if (msaa) {
		// Resolving MSAA requires these flags:
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
	} else {
		// Make this a staging resource to save DirectXTK having to create it's
		// own staging resource.
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	}

	// Clear out bind flags that may prevent the copy from working:
	desc.BindFlags = 0;

	// Mip maps requires bind flags, but we set them to 0:
	// XXX: Possibly want a whilelist instead? DirectXTK only allows
	// D3D11_RESOURCE_MISC_TEXTURECUBE
	desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;

	// Must resolve MSAA surfaces before the reverse stereo blit:
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;

	// Force surface creation mode to stereo to prevent driver heuristics
	// interfering. If the original surface was mono that's ok - thaks to
	// the intermediate stages we'll end up with both eyes the same
	// (without this one eye would be blank instead, which is arguably
	// better since it will be immediately obvious, but risks missing the
	// second perspective if the original resource was actually stereo)
	NvAPI_Stereo_GetSurfaceCreationMode(mHackerDevice->mStereoHandle, &orig_mode);
	NvAPI_Stereo_SetSurfaceCreationMode(mHackerDevice->mStereoHandle, NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO);

	hr = mOrigDevice1->CreateTexture2D(&desc, NULL, resource);

	NvAPI_Stereo_SetSurfaceCreationMode(mHackerDevice->mStereoHandle, orig_mode);
	return hr;
}

// TODO: Refactor this with StereoScreenShot().
// Expects the reverse stereo blit to be enabled by the caller
void HackerContext::DumpStereoResource(ID3D11Texture2D *resource, wchar_t *filename,
		FrameAnalysisOptions type_mask)
{
	ID3D11Texture2D *stereoResource = NULL;
	ID3D11Texture2D *tmpResource = NULL;
	ID3D11Texture2D *tmpResource2 = NULL;
	ID3D11Texture2D *src = resource;
	D3D11_TEXTURE2D_DESC srcDesc;
	D3D11_BOX srcBox;
	HRESULT hr;
	UINT item, level, index, width, height;

	resource->GetDesc(&srcDesc);

	hr = CreateStagingResource(&stereoResource, srcDesc, true, false);
	if (FAILED(hr)) {
		FALogInfo("DumpStereoResource failed to create stereo texture: 0x%x\n", hr);
		return;
	}

	if ((srcDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) ||
	    (srcDesc.SampleDesc.Count > 1)) {
		// Reverse stereo blit won't work on these surfaces directly
		// since CopySubresourceRegion() will fail if the source and
		// destination dimensions don't match, so use yet another
		// intermediate staging resource first.
		hr = CreateStagingResource(&tmpResource, srcDesc, false, false);
		if (FAILED(hr)) {
			FALogInfo("DumpStereoResource failed to create intermediate texture: 0x%x\n", hr);
			goto out;
		}

		if (srcDesc.SampleDesc.Count > 1) {
			// Resolve MSAA surfaces. Procedure copied from DirectXTK
			// These need to have D3D11_USAGE_DEFAULT to resolve,
			// so we need yet another intermediate texture:
			hr = CreateStagingResource(&tmpResource2, srcDesc, false, true);
			if (FAILED(hr)) {
				FALogInfo("DumpStereoResource failed to create intermediate texture: 0x%x\n", hr);
				goto out1;
			}

			DXGI_FORMAT fmt = EnsureNotTypeless(srcDesc.Format);
			UINT support = 0;

			hr = mOrigDevice1->CheckFormatSupport( fmt, &support );
			if (FAILED(hr) || !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) {
				FALogInfo("DumpStereoResource cannot resolve MSAA format %d\n", fmt);
				goto out2;
			}

			for (item = 0; item < srcDesc.ArraySize; item++) {
				for (level = 0; level < srcDesc.MipLevels; level++) {
					index = D3D11CalcSubresource(level, item, max(srcDesc.MipLevels, 1));
					mPassThroughContext1->ResolveSubresource(tmpResource2, index, src, index, fmt);
				}
			}
			src = tmpResource2;
		}

		mPassThroughContext1->CopyResource(tmpResource, src);
		src = tmpResource;
	}

	// Set the source box as per the nvapi documentation:
	srcBox.left = 0;
	srcBox.top = 0;
	srcBox.front = 0;
	srcBox.right = width = srcDesc.Width;
	srcBox.bottom = height = srcDesc.Height;
	srcBox.back = 1;

	// Perform the reverse stereo blit on all sub-resources and mip-maps:
	for (item = 0; item < srcDesc.ArraySize; item++) {
		for (level = 0; level < srcDesc.MipLevels; level++) {
			index = D3D11CalcSubresource(level, item, max(srcDesc.MipLevels, 1));
			srcBox.right = width >> level;
			srcBox.bottom = height >> level;
			mPassThroughContext1->CopySubresourceRegion(stereoResource, index, 0, 0, 0,
					src, index, &srcBox);
		}
	}

	Dump2DResource(stereoResource, filename, true, type_mask);

out2:
	if (tmpResource2)
		tmpResource2->Release();
out1:
	if (tmpResource)
		tmpResource->Release();

out:
	stereoResource->Release();
}

/*
 * This just treats the buffer as an array of float4s. In the future we might
 * try to use the reflection information in the shaders to add names and
 * correct types.
 */
void HackerContext::DumpBufferTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, char type, int idx, UINT stride, UINT offset)
{
	FILE *fd = NULL;
	char *components = "xyzw";
	float *buf = (float*)map->pData;
	UINT i, c;
	errno_t err;

	err = _wfopen_s(&fd, filename, L"w");
	if (!fd) {
		FALogInfo("Unable to create %S: %u\n", filename, err);
		return;
	}

	if (offset)
		fprintf(fd, "offset: %u\n", offset);
	if (stride)
		fprintf(fd, "stride: %u\n", stride);

	for (i = offset / 16; i < size / 16; i++) {
		for (c = offset % 4; c < 4; c++) {
			if (idx == -1)
				fprintf(fd, "buf[%d].%c: %.9g\n", i, components[c], buf[i*4+c]);
			else
				fprintf(fd, "%cb%i[%d].%c: %.9g\n", type, idx, i, components[c], buf[i*4+c]);
		}
	}

	fclose(fd);
}

/*
 * Dumps the vertex buffer in several formats.
 * FIXME: We should wrap the input layout object to get the correct format (and
 * other info like the semantic).
 */
void HackerContext::DumpVBTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, int idx, UINT stride, UINT offset, UINT first, UINT count)
{
	FILE *fd = NULL;
	float *buff = (float*)map->pData;
	int *buf32 = (int*)map->pData;
	char *buf8 = (char*)map->pData;
	UINT i, j, start, end, buf_idx;
	errno_t err;

	err = _wfopen_s(&fd, filename, L"w");
	if (!fd) {
		FALogInfo("Unable to create %S: %u\n", filename, err);
		return;
	}

	if (offset)
		fprintf(fd, "byte offset: %u\n", offset);
	fprintf(fd, "stride: %u\n", stride);
	if (first || count) {
		fprintf(fd, "first vertex: %u\n", first);
		fprintf(fd, "vertex count: %u\n", count);
	}
	if (!stride) {
		FALogInfo("Cannot dump vertex buffer with stride=0\n");
		return;
	}

	start = offset / stride + first;
	end = size / stride;
	if (count)
		end = min(end, start + count);

	// FIXME: For vertex buffers we should wrap the input layout object to
	// get the format (and other info like the semantic).

	for (i = start; i < end; i++) {
		fprintf(fd, "\n");
		for (j = 0; j < stride / 4; j++) {
			buf_idx = i * stride / 4 + j;
			fprintf(fd, "vb%i[%d]+%03d: 0x%08x %.9g\n", idx, i - start, j*4, buf32[buf_idx], buff[buf_idx]);
		}
		// In case we find one that is not a 32bit multiple finish off one byte at a time:
		for (j = j * 4; j < stride; j++) {
			buf_idx = i * stride + j;
			fprintf(fd, "vb%i[%d]+%03d: 0x%02x\n", idx, i - start, j, buf8[buf_idx]);
		}
	}

	fclose(fd);
}

void HackerContext::DumpIBTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, DXGI_FORMAT format, UINT offset, UINT first, UINT count)
{
	FILE *fd = NULL;
	short *buf16 = (short*)map->pData;
	int *buf32 = (int*)map->pData;
	UINT start, end, i;
	errno_t err;

	err = _wfopen_s(&fd, filename, L"w");
	if (!fd) {
		FALogInfo("Unable to create %S: %u\n", filename, err);
		return;
	}

	fprintf(fd, "byte offset: %u\n", offset);
	if (first || count) {
		fprintf(fd, "first index: %u\n", first);
		fprintf(fd, "index count: %u\n", count);
	}

	switch(format) {
	case DXGI_FORMAT_R16_UINT:
		fprintf(fd, "format: DXGI_FORMAT_R16_UINT\n");

		start = offset / 2 + first;
		end = size / 2;
		if (count)
			end = min(end, start + count);

		for (i = start; i < end; i++)
			fprintf(fd, "%u\n", buf16[i]);
		break;
	case DXGI_FORMAT_R32_UINT:
		fprintf(fd, "format: DXGI_FORMAT_R32_UINT\n");

		start = offset / 4 + first;
		end = size / 4;
		if (count)
			end = min(end, start + count);

		for (i = start; i < end; i++)
			fprintf(fd, "%u\n", buf32[i]);
		break;
	default:
		// Illegal format for an index buffer
		fprintf(fd, "format %u is illegal\n", format);
		break;
	}

	fclose(fd);
}

void HackerContext::DumpBuffer(ID3D11Buffer *buffer, wchar_t *filename,
		FrameAnalysisOptions type_mask, int idx, DXGI_FORMAT ib_fmt,
		UINT stride, UINT offset, UINT first, UINT count)
{
	FrameAnalysisOptions options = (FrameAnalysisOptions)(analyse_options & type_mask);
	D3D11_BUFFER_DESC desc;
	D3D11_MAPPED_SUBRESOURCE map;
	ID3D11Buffer *staging = NULL;
	HRESULT hr;
	FILE *fd = NULL;
	wchar_t *ext;
	errno_t err;

	ext = wcsrchr(filename, L'.');
	if (!ext) {
		FALogInfo("DumpBuffer: Filename missing extension\n");
		return;
	}

	buffer->GetDesc(&desc);

	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.MiscFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	hr = mOrigDevice1->CreateBuffer(&desc, NULL, &staging);
	if (FAILED(hr)) {
		FALogInfo("DumpBuffer failed to create staging buffer: 0x%x\n", hr);
		return;
	}

	mPassThroughContext1->CopyResource(staging, buffer);
	hr = mPassThroughContext1->Map(staging, 0, D3D11_MAP_READ, 0, &map);
	if (FAILED(hr)) {
		FALogInfo("DumpBuffer failed to map staging resource: 0x%x\n", hr);
		return;
	}

	if (options & FrameAnalysisOptions::DUMP_XX_BIN) {
		wcscpy_s(ext, MAX_PATH + filename - ext, L".buf");

		err = _wfopen_s(&fd, filename, L"wb");
		if (!fd) {
			FALogInfo("Unable to create %S: %u\n", filename, err);
			goto out_unmap;
		}
		fwrite(map.pData, 1, desc.ByteWidth, fd);
		fclose(fd);
	}

	if (options & FrameAnalysisOptions::DUMP_XX_TXT) {
		wcscpy_s(ext, MAX_PATH + filename - ext, L".txt");
		if (options & FrameAnalysisOptions::DUMP_CB_TXT)
			DumpBufferTxt(filename, &map, desc.ByteWidth, 'c', idx, stride, offset);
		else if (options & FrameAnalysisOptions::DUMP_VB_TXT)
			DumpVBTxt(filename, &map, desc.ByteWidth, idx, stride, offset, first, count);
		else if (options & FrameAnalysisOptions::DUMP_IB_TXT)
			DumpIBTxt(filename, &map, desc.ByteWidth, ib_fmt, offset, first, count);
		else if (options & FrameAnalysisOptions::DUMP_ON_XXXXXX) {
			// We don't know what kind of buffer this is, so just
			// use the generic dump routine:
			DumpBufferTxt(filename, &map, desc.ByteWidth, '?', idx, stride, offset);
		}
	}
	// TODO: Dump UAV, RT and SRV buffers as text taking their format,
	// offset, size, first entry and num entries into account.

out_unmap:
	mPassThroughContext1->Unmap(staging, 0);
	staging->Release();
}

void HackerContext::DumpResource(ID3D11Resource *resource, wchar_t *filename,
		FrameAnalysisOptions type_mask, int idx, DXGI_FORMAT ib_fmt,
		UINT stride, UINT offset)
{
	D3D11_RESOURCE_DIMENSION dim;

	resource->GetType(&dim);

	switch (dim) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			DumpBuffer((ID3D11Buffer*)resource, filename, type_mask, idx, ib_fmt, stride, offset, 0, 0);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			FALogInfo("Skipped dumping Texture1D resource\n");
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			if (analyse_options & FrameAnalysisOptions::STEREO)
				DumpStereoResource((ID3D11Texture2D*)resource, filename, type_mask);
			if (analyse_options & FrameAnalysisOptions::MONO)
				Dump2DResource((ID3D11Texture2D*)resource, filename, false, type_mask);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			FALogInfo("Skipped dumping Texture3D resource\n");
			break;
		default:
			FALogInfo("Skipped dumping resource of unknown type %i\n", dim);
			break;
	}
}

HRESULT HackerContext::FrameAnalysisFilename(wchar_t *filename, size_t size, bool compute,
		wchar_t *reg, char shader_type, int idx, uint32_t hash, uint32_t orig_hash,
		ID3D11Resource *handle)
{
	struct ResourceHashInfo *info;
	wchar_t *pos;
	size_t rem;
	HRESULT hr;

	StringCchPrintfExW(filename, size, &pos, &rem, NULL, L"%ls\\", G->ANALYSIS_PATH);

	if (!(analyse_options & FrameAnalysisOptions::FILENAME_REG)) {
		// We don't allow hold to be changed mid-frame due to potential
		// for filename conflicts, so use def_analyse_options:
		if (G->def_analyse_options & FrameAnalysisOptions::HOLD)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%i.", G->analyse_frame_no);
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%06i-", G->analyse_frame);
	}

	if (shader_type)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%cs-", shader_type);

	StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%ls", reg);
	if (idx != -1)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%i", idx);

	if (analyse_options & FrameAnalysisOptions::FILENAME_REG) {
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-");
		// We don't allow hold to be changed mid-frame due to potential
		// for filename conflicts, so use def_analyse_options:
		if (G->def_analyse_options & FrameAnalysisOptions::HOLD)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%i.", G->analyse_frame_no);
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%06i", G->analyse_frame);
	}

	if (hash) {
		try {
			info = &G->mResourceInfo.at(orig_hash);
			if (info->hash_contaminated) {
				StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"=!");
				if (!info->map_contamination.empty())
					StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"M");
				if (!info->update_contamination.empty())
					StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"U");
				if (!info->copy_contamination.empty())
					StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"C");
				if (!info->region_contamination.empty())
					StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"S");
				StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"!");
			}
		} catch (std::out_of_range) {}

		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"=%08x", hash);

		if (hash != orig_hash)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"(%08x)", orig_hash);
	}
	if (analyse_options & FrameAnalysisOptions::FILENAME_HANDLE)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"@%p", handle);

	if (compute) {
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-cs=%016I64x", mCurrentComputeShader);
	} else {
		if (mCurrentVertexShader)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-vs=%016I64x", mCurrentVertexShader);
		if (mCurrentHullShader)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-hs=%016I64x", mCurrentHullShader);
		if (mCurrentDomainShader)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-ds=%016I64x", mCurrentDomainShader);
		if (mCurrentGeometryShader)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-gs=%016I64x", mCurrentGeometryShader);
		if (mCurrentPixelShader)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"-ps=%016I64x", mCurrentPixelShader);
	}

	hr = StringCchPrintfW(pos, rem, L".XXX");
	if (FAILED(hr)) {
		FALogInfo("Failed to create filename: 0x%x\n", hr);
		// Could create a shorter filename without hashes if this
		// becomes a problem in practice
	}

	return hr;
}

HRESULT HackerContext::FrameAnalysisFilenameResource(wchar_t *filename, size_t size, wchar_t *type,
		uint32_t hash, uint32_t orig_hash, ID3D11Resource *handle)
{
	struct ResourceHashInfo *info;
	wchar_t *pos;
	size_t rem;
	HRESULT hr;

	StringCchPrintfExW(filename, size, &pos, &rem, NULL, L"%ls\\", G->ANALYSIS_PATH);

	// We don't allow hold to be changed mid-frame due to potential
	// for filename conflicts, so use def_analyse_options:
	if (G->def_analyse_options & FrameAnalysisOptions::HOLD)
		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%i.", G->analyse_frame_no);
	StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%06i-", G->analyse_frame);

	StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"%s-", type);

	if (hash) {
		try {
			info = &G->mResourceInfo.at(orig_hash);
			if (info->hash_contaminated) {
				StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"=!");
				if (!info->map_contamination.empty())
					StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"M");
				if (!info->update_contamination.empty())
					StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"U");
				if (!info->copy_contamination.empty())
					StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"C");
				if (!info->region_contamination.empty())
					StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"S");
				StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"!");
			}
		} catch (std::out_of_range) {}

		StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"=%08x", hash);

		if (hash != orig_hash)
			StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"(%08x)", orig_hash);
	}

	// Always do this for resource dumps since hashes are likely to clash:
	StringCchPrintfExW(pos, rem, &pos, &rem, NULL, L"@%p", handle);

	hr = StringCchPrintfW(pos, rem, L".XXX");
	if (FAILED(hr))
		FALogInfo("Failed to create filename: 0x%x\n", hr);

	return hr;
}

void HackerContext::_DumpCBs(char shader_type, bool compute,
	ID3D11Buffer *buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT])
{
	wchar_t filename[MAX_PATH];
	HRESULT hr;
	UINT i;

	for (i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; i++) {
		if (!buffers[i])
			continue;

		hr = FrameAnalysisFilename(filename, MAX_PATH, compute, L"cb", shader_type, i, 0, 0, buffers[i]);
		if (SUCCEEDED(hr)) {
			DumpResource(buffers[i], filename,
					FrameAnalysisOptions::DUMP_CB_MASK, i,
					DXGI_FORMAT_UNKNOWN, 0, 0);
		}

		buffers[i]->Release();
	}
}

void HackerContext::_DumpTextures(char shader_type, bool compute,
	ID3D11ShaderResourceView *views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT])
{
	ID3D11Resource *resource;
	D3D11_RESOURCE_DIMENSION dim;
	wchar_t filename[MAX_PATH];
	uint32_t hash, orig_hash;
	HRESULT hr;
	UINT i;

	for (i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++) {
		if (!views[i])
			continue;

		views[i]->GetResource(&resource);
		if (!resource) {
			views[i]->Release();
			continue;
		}

		resource->GetType(&dim);

		try {
			hash = G->mResources.at((ID3D11Texture2D *)resource).hash;
			orig_hash = G->mResources.at((ID3D11Texture2D *)resource).orig_hash;
		} catch (std::out_of_range) {
			hash = orig_hash = 0;
		}

		// TODO: process description to get offset, strides & size for
		// buffer & bufferex type SRVs and pass down to dump routines,
		// although I have no idea how to determine which of the
		// entries in the two D3D11_BUFFER_SRV unions will be valid.

		hr = FrameAnalysisFilename(filename, MAX_PATH, compute, L"t", shader_type, i, hash, orig_hash, resource);
		if (SUCCEEDED(hr)) {
			DumpResource(resource, filename,
					FrameAnalysisOptions::DUMP_TEX_MASK, i,
					DXGI_FORMAT_UNKNOWN, 0, 0);
		}

		resource->Release();
		views[i]->Release();
	}
}

void HackerContext::DumpCBs(bool compute)
{
	ID3D11Buffer *buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];

	if (compute) {
		if (mCurrentComputeShader) {
			mPassThroughContext1->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
			_DumpCBs('c', compute, buffers);
		}
	} else {
		if (mCurrentVertexShader) {
			mPassThroughContext1->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
			_DumpCBs('v', compute, buffers);
		}
		if (mCurrentHullShader) {
			mPassThroughContext1->HSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
			_DumpCBs('h', compute, buffers);
		}
		if (mCurrentDomainShader) {
			mPassThroughContext1->DSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
			_DumpCBs('d', compute, buffers);
		}
		if (mCurrentGeometryShader) {
			mPassThroughContext1->GSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
			_DumpCBs('g', compute, buffers);
		}
		if (mCurrentPixelShader) {
			mPassThroughContext1->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
			_DumpCBs('p', compute, buffers);
		}
	}
}

void HackerContext::DumpVBs(DrawCallInfo *call_info)
{
	ID3D11Buffer *buffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	wchar_t filename[MAX_PATH];
	HRESULT hr;
	UINT i, first = 0, count = 0;

	if (call_info) {
		first = call_info->FirstVertex;
		count = call_info->VertexCount;
	}

	// TODO: The format of each vertex buffer cannot be obtained from this
	// call. Rather, it is available in the input layout assigned to the
	// pipeline. There is no API to get the layout description, so if we
	// want to obtain it we will need to wrap the input layout objects
	// (there may be other good reasons to consider wrapping the input
	// layout if we ever do anything advanced with the vertex buffers).

	mPassThroughContext1->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, buffers, strides, offsets);

	for (i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++) {
		if (!buffers[i])
			continue;

		hr = FrameAnalysisFilename(filename, MAX_PATH, false, L"vb", NULL, i, 0, 0, buffers[i]);
		if (SUCCEEDED(hr)) {
			DumpBuffer(buffers[i], filename,
				FrameAnalysisOptions::DUMP_VB_MASK, i,
				DXGI_FORMAT_UNKNOWN, strides[i], offsets[i],
				first, count);
		}

		buffers[i]->Release();
	}
}

void HackerContext::DumpIB(DrawCallInfo *call_info)
{
	ID3D11Buffer *buffer = NULL;
	wchar_t filename[MAX_PATH];
	HRESULT hr;
	DXGI_FORMAT format;
	UINT offset, first = 0, count = 0;

	if (call_info) {
		first = call_info->FirstIndex;
		count = call_info->IndexCount;
	}

	mPassThroughContext1->IAGetIndexBuffer(&buffer, &format, &offset);
	if (!buffer)
		return;

	hr = FrameAnalysisFilename(filename, MAX_PATH, false, L"ib", NULL, -1, 0, 0, buffer);
	if (SUCCEEDED(hr)) {
		DumpBuffer(buffer, filename,
				FrameAnalysisOptions::DUMP_IB_MASK, -1,
				format, 0, offset, first, count);
	}

	buffer->Release();
}

void HackerContext::DumpTextures(bool compute)
{
	ID3D11ShaderResourceView *views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];

	if (compute) {
		if (mCurrentComputeShader) {
			mPassThroughContext1->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
			_DumpTextures('c', compute, views);
		}
	} else {
		if (mCurrentVertexShader) {
			mPassThroughContext1->VSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
			_DumpTextures('v', compute, views);
		}
		if (mCurrentHullShader) {
			mPassThroughContext1->HSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
			_DumpTextures('h', compute, views);
		}
		if (mCurrentDomainShader) {
			mPassThroughContext1->DSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
			_DumpTextures('d', compute, views);
		}
		if (mCurrentGeometryShader) {
			mPassThroughContext1->GSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
			_DumpTextures('g', compute, views);
		}
		if (mCurrentPixelShader) {
			mPassThroughContext1->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
			_DumpTextures('p', compute, views);
		}
	}
}

void HackerContext::DumpRenderTargets()
{
	UINT i;
	wchar_t filename[MAX_PATH];
	HRESULT hr;
	uint32_t hash, orig_hash;

	for (i = 0; i < mCurrentRenderTargets.size(); ++i) {
		// TODO: Decouple from HackerContext and remove dependency on
		// stat collection by querying the DeviceContext directly like
		// we do for all other resources
		try {
			hash = G->mResources.at(mCurrentRenderTargets[i]).hash;
			orig_hash = G->mResources.at(mCurrentRenderTargets[i]).orig_hash;
		} catch (std::out_of_range) {
			hash = orig_hash = 0;
		}

		// TODO: process description to get offset, strides & size for
		// buffer type RTVs and pass down to dump routines, although I
		// have no idea how to determine which of the entries in the
		// two D3D11_BUFFER_RTV unions will be valid.

		hr = FrameAnalysisFilename(filename, MAX_PATH, false, L"o", NULL, i,
				hash, orig_hash, (ID3D11Resource*)mCurrentRenderTargets[i]);
		if (FAILED(hr))
			return;
		DumpResource((ID3D11Resource*)mCurrentRenderTargets[i], filename,
				FrameAnalysisOptions::DUMP_RT_MASK, i,
				DXGI_FORMAT_UNKNOWN, 0, 0);
	}
}

void HackerContext::DumpDepthStencilTargets()
{
	wchar_t filename[MAX_PATH];
	HRESULT hr;
	uint32_t hash, orig_hash;

	if (mCurrentDepthTarget) {
		// TODO: Decouple from HackerContext and remove dependency on
		// stat collection by querying the DeviceContext directly like
		// we do for all other resources
		try {
			hash = G->mResources.at(mCurrentDepthTarget).hash;
			orig_hash = G->mResources.at(mCurrentDepthTarget).orig_hash;
		} catch (std::out_of_range) {
			hash = orig_hash = 0;
		}

		hr = FrameAnalysisFilename(filename, MAX_PATH, false, L"oD", NULL, -1,
				hash, orig_hash, (ID3D11Resource*)mCurrentDepthTarget);
		if (FAILED(hr))
			return;
		DumpResource((ID3D11Resource*)mCurrentDepthTarget, filename,
				FrameAnalysisOptions::DUMP_DEPTH_MASK, -1,
				DXGI_FORMAT_UNKNOWN, 0, 0);
	}
}

void HackerContext::DumpUAVs(bool compute)
{
	UINT i;
	ID3D11UnorderedAccessView *uavs[D3D11_PS_CS_UAV_REGISTER_COUNT];
	ID3D11Resource *resource;
	wchar_t filename[MAX_PATH];
	HRESULT hr;
	uint32_t hash, orig_hash;

	if (compute)
		mPassThroughContext1->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
	else
		mPassThroughContext1->OMGetRenderTargetsAndUnorderedAccessViews(0, NULL, NULL, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);

	for (i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) {
		if (!uavs[i])
			continue;

		uavs[i]->GetResource(&resource);
		if (!resource) {
			uavs[i]->Release();
			continue;
		}

		try {
			hash = G->mResources.at(resource).hash;
			orig_hash = G->mResources.at(resource).orig_hash;
		} catch (std::out_of_range) {
			hash = orig_hash = 0;
		}

		// TODO: process description to get offset & size for buffer
		// type UAVs and pass down to dump routines.

		hr = FrameAnalysisFilename(filename, MAX_PATH, compute, L"u", NULL, i, hash, orig_hash, resource);
		if (SUCCEEDED(hr)) {
			DumpResource(resource, filename,
					FrameAnalysisOptions::DUMP_RT_MASK, i,
					DXGI_FORMAT_UNKNOWN, 0, 0);
		}

		resource->Release();
		uavs[i]->Release();
	}
}

void HackerContext::FrameAnalysisClearRT(ID3D11RenderTargetView *target)
{
	FLOAT colour[4] = {0,0,0,0};
	ID3D11Resource *resource = NULL;

	// FIXME: Do this before each draw call instead of when render targets
	// are assigned to fix assigned render targets not being cleared, and
	// work better with frame analysis triggers

	if (!(G->cur_analyse_options & FrameAnalysisOptions::CLEAR_RT))
		return;

	// Use the address of the resource rather than the view to determine if
	// we have seen it before so we don't clear a render target that is
	// simply used as several different types of views:
	target->GetResource(&resource);
	if (!resource)
		return;
	resource->Release(); // Don't need the object, only the address

	if (G->frame_analysis_seen_rts.count(resource))
		return;
	G->frame_analysis_seen_rts.insert(resource);

	mPassThroughContext1->ClearRenderTargetView(target, colour);
}

void HackerContext::FrameAnalysisClearUAV(ID3D11UnorderedAccessView *uav)
{
	UINT values[4] = {0,0,0,0};
	ID3D11Resource *resource = NULL;

	// FIXME: Do this before each draw/dispatch call instead of when UAVs
	// are assigned to fix assigned render targets not being cleared, and
	// work better with frame analysis triggers

	if (!(G->cur_analyse_options & FrameAnalysisOptions::CLEAR_RT))
		return;

	// Use the address of the resource rather than the view to determine if
	// we have seen it before so we don't clear a render target that is
	// simply used as several different types of views:
	uav->GetResource(&resource);
	if (!resource)
		return;
	resource->Release(); // Don't need the object, only the address

	if (G->frame_analysis_seen_rts.count(resource))
		return;
	G->frame_analysis_seen_rts.insert(resource);

	mPassThroughContext1->ClearUnorderedAccessViewUint(uav, values);
}

void HackerContext::FrameAnalysisProcessTriggers(bool compute)
{
	FrameAnalysisOptions new_options = FrameAnalysisOptions::INVALID;
	struct ShaderOverride *shaderOverride;
	struct TextureOverride *textureOverride;
	uint32_t hash;
	UINT i;

	// TODO: Trigger on texture inputs

	if (compute) {
		try {
			shaderOverride = &G->mShaderOverrideMap.at(mCurrentComputeShader);
			new_options |= shaderOverride->analyse_options;
		} catch (std::out_of_range) {}

		// TODO: Trigger on current UAVs
	} else {
		try {
			shaderOverride = &G->mShaderOverrideMap.at(mCurrentVertexShader);
			new_options |= shaderOverride->analyse_options;
		} catch (std::out_of_range) {}

		try {
			shaderOverride = &G->mShaderOverrideMap.at(mCurrentHullShader);
			new_options |= shaderOverride->analyse_options;
		} catch (std::out_of_range) {}

		try {
			shaderOverride = &G->mShaderOverrideMap.at(mCurrentDomainShader);
			new_options |= shaderOverride->analyse_options;
		} catch (std::out_of_range) {}

		try {
			shaderOverride = &G->mShaderOverrideMap.at(mCurrentGeometryShader);
			new_options |= shaderOverride->analyse_options;
		} catch (std::out_of_range) {}

		try {
			shaderOverride = &G->mShaderOverrideMap.at(mCurrentPixelShader);
			new_options |= shaderOverride->analyse_options;
		} catch (std::out_of_range) {}

		for (i = 0; i < mCurrentRenderTargets.size(); ++i) {
			try {
				hash = G->mResources.at(mCurrentRenderTargets[i]).hash;
				textureOverride = &G->mTextureOverrideMap.at(hash);
				new_options |= textureOverride->analyse_options;
			} catch (std::out_of_range) {}
		}

		if (mCurrentDepthTarget) {
			try {
				hash = G->mResources.at(mCurrentDepthTarget).hash;
				textureOverride = &G->mTextureOverrideMap.at(hash);
				new_options |= textureOverride->analyse_options;
			} catch (std::out_of_range) {}
		}
	}

	if (!new_options)
		return;

	analyse_options = new_options;

	if (new_options & FrameAnalysisOptions::PERSIST) {
		G->cur_analyse_options = new_options;
		FALogInfo("analyse_options (persistent): %08x\n", new_options);
	} else
		FALogInfo("analyse_options (one-shot): %08x\n", new_options);
}

void HackerContext::FrameAnalysisAfterDraw(bool compute, DrawCallInfo *call_info)
{
	NvAPI_Status nvret;

	// Bail if we are a deferred context, as there will not be anything to
	// dump out yet and we don't want to alter the global draw count. Later
	// we might want to think about ways we could analyse deferred contexts
	// - a simple approach would be to dump out the back buffer after
	// executing a command list in the immediate context, however this
	// would only show the combined result of all the draw calls from the
	// deferred context, and not the results of the individual draw
	// operations.
	//
	// Another more in-depth approach would be to create the stereo
	// resources now and issue the reverse blits, then dump them all after
	// executing the command list. Note that the NVAPI call is not
	// per-context and therefore may have threading issues, and it's not
	// clear if it would have to be enabled while submitting the copy
	// commands in the deferred context, or while playing the command queue
	// in the immediate context, or both.
	if (mPassThroughContext1->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
		return;

	analyse_options = G->cur_analyse_options;

	FrameAnalysisProcessTriggers(compute);

	// If neither stereo or mono specified, default to stereo:
	if (!(analyse_options & FrameAnalysisOptions::STEREO_MASK))
		analyse_options |= FrameAnalysisOptions::STEREO;

	if ((analyse_options & FrameAnalysisOptions::DUMP_XXX_MASK) &&
	    (analyse_options & FrameAnalysisOptions::STEREO)) {
		// Enable reverse stereo blit for all resources we are about to dump:
		nvret = NvAPI_Stereo_ReverseStereoBlitControl(mHackerDevice->mStereoHandle, true);
		if (nvret != NVAPI_OK) {
			FALogInfo("DumpStereoResource failed to enable reverse stereo blit\n");
			// Continue anyway, we should still be able to dump in 2D...
		}
	}

	// Grab the critical section now as we may need it several times during
	// dumping for mResources
	EnterCriticalSection(&G->mCriticalSection);

	if (analyse_options & FrameAnalysisOptions::DUMP_CB_MASK)
		DumpCBs(compute);

	if (!compute) {
		if (analyse_options & FrameAnalysisOptions::DUMP_VB_MASK)
			DumpVBs(call_info);

		if (analyse_options & FrameAnalysisOptions::DUMP_IB_MASK)
			DumpIB(call_info);
	}

	if (analyse_options & FrameAnalysisOptions::DUMP_TEX_MASK)
		DumpTextures(compute);

	if (analyse_options & FrameAnalysisOptions::DUMP_RT_MASK) {
		if (!compute)
			DumpRenderTargets();

		// UAVs can be used by both pixel shaders and compute shaders:
		DumpUAVs(compute);
	}

	if (analyse_options & FrameAnalysisOptions::DUMP_DEPTH_MASK && !compute)
		DumpDepthStencilTargets();

	LeaveCriticalSection(&G->mCriticalSection);

	if ((analyse_options & FrameAnalysisOptions::DUMP_XXX_MASK) &&
	    (analyse_options & FrameAnalysisOptions::STEREO)) {
		NvAPI_Stereo_ReverseStereoBlitControl(mHackerDevice->mStereoHandle, false);
	}

	G->analyse_frame++;
}

void HackerContext::_FrameAnalysisAfterUpdate(ID3D11Resource *resource,
		FrameAnalysisOptions type_mask, wchar_t *type)
{
	wchar_t filename[MAX_PATH];
	uint32_t hash = 0, orig_hash = 0;
	HRESULT hr;

	analyse_options = G->cur_analyse_options;

	if (!(analyse_options & type_mask))
		return;

	// Don't bother trying to dump as stereo - Map/Unmap/Update are inherently mono
	analyse_options &= (FrameAnalysisOptions)~FrameAnalysisOptions::STEREO_MASK;
	analyse_options |= FrameAnalysisOptions::MONO;

	EnterCriticalSection(&G->mCriticalSection);

	try {
		hash = G->mResources.at(resource).hash;
		orig_hash = G->mResources.at(resource).orig_hash;
	} catch (std::out_of_range) {
	}

	hr = FrameAnalysisFilenameResource(filename, MAX_PATH, type, hash, orig_hash, resource);
	if (SUCCEEDED(hr)) {
		DumpResource(resource, filename, type_mask, -1, DXGI_FORMAT_UNKNOWN, 0, 0);
	}

	LeaveCriticalSection(&G->mCriticalSection);

	// XXX: Might be better to use a second counter for these
	G->analyse_frame++;
}

void HackerContext::FrameAnalysisAfterUnmap(ID3D11Resource *resource)
{
	_FrameAnalysisAfterUpdate(resource, FrameAnalysisOptions::DUMP_ON_UNMAP, L"unmap");
}

void HackerContext::FrameAnalysisAfterUpdate(ID3D11Resource *resource)
{
	_FrameAnalysisAfterUpdate(resource, FrameAnalysisOptions::DUMP_ON_UPDATE, L"update");
}

// -----------------------------------------------------------------------------------------------

ULONG STDMETHODCALLTYPE FrameAnalysisContext::AddRef(void)
{
	return mOrigContext->AddRef();
}

STDMETHODIMP_(ULONG) FrameAnalysisContext::Release(THIS)
{
	return mOrigContext->Release();
}

HRESULT STDMETHODCALLTYPE FrameAnalysisContext::QueryInterface(
		/* [in] */ REFIID riid,
		/* [iid_is][out] */ _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject)
{
	return mOrigContext->QueryInterface(riid, ppvObject);
}

STDMETHODIMP_(void) FrameAnalysisContext::GetDevice(THIS_
		/* [annotation] */
		__out  ID3D11Device **ppDevice)
{
	return mOrigContext->GetDevice(ppDevice);
}

STDMETHODIMP FrameAnalysisContext::GetPrivateData(THIS_
		/* [annotation] */
		__in  REFGUID guid,
		/* [annotation] */
		__inout  UINT *pDataSize,
		/* [annotation] */
		__out_bcount_opt(*pDataSize)  void *pData)
{
	return mOrigContext->GetPrivateData(guid, pDataSize, pData);
}

STDMETHODIMP FrameAnalysisContext::SetPrivateData(THIS_
		/* [annotation] */
		__in  REFGUID guid,
		/* [annotation] */
		__in  UINT DataSize,
		/* [annotation] */
		__in_bcount_opt(DataSize)  const void *pData)
{
	return mOrigContext->SetPrivateData(guid, DataSize, pData);
}

STDMETHODIMP FrameAnalysisContext::SetPrivateDataInterface(THIS_
		/* [annotation] */
		__in  REFGUID guid,
		/* [annotation] */
		__in_opt  const IUnknown *pData)
{
	return mOrigContext->SetPrivateDataInterface(guid, pData);
}

STDMETHODIMP_(void) FrameAnalysisContext::VSSetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__in_ecount(NumBuffers) ID3D11Buffer *const *ppConstantBuffers)
{
	FrameAnalysisLog("VSSetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP FrameAnalysisContext::Map(THIS_
		/* [annotation] */
		__in  ID3D11Resource *pResource,
		/* [annotation] */
		__in  UINT Subresource,
		/* [annotation] */
		__in  D3D11_MAP MapType,
		/* [annotation] */
		__in  UINT MapFlags,
		/* [annotation] */
		__out D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
	FrameAnalysisLogNoNL("Map(pResource:0x%p, Subresource:%u, MapType:%u, MapFlags:%u, pMappedResource:0x%p)",
			pResource, Subresource, MapType, MapFlags, pMappedResource);
	FrameAnalysisLogResourceHash(pResource);

	return mOrigContext->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
}

STDMETHODIMP_(void) FrameAnalysisContext::Unmap(THIS_
		/* [annotation] */
		__in ID3D11Resource *pResource,
		/* [annotation] */
		__in  UINT Subresource)
{
	FrameAnalysisLogNoNL("Unmap(pResource:0x%p, Subresource:%u)",
			pResource, Subresource);
	FrameAnalysisLogResourceHash(pResource);

	mOrigContext->Unmap(pResource, Subresource);
}

STDMETHODIMP_(void) FrameAnalysisContext::PSSetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__in_ecount(NumBuffers) ID3D11Buffer *const *ppConstantBuffers)
{
	FrameAnalysisLog("PSSetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::IASetInputLayout(THIS_
		/* [annotation] */
		__in_opt ID3D11InputLayout *pInputLayout)
{
	FrameAnalysisLog("IASetInputLayout(pInputLayout:0x%p)\n",
			pInputLayout);

	mOrigContext->IASetInputLayout(pInputLayout);
}

STDMETHODIMP_(void) FrameAnalysisContext::IASetVertexBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__in_ecount(NumBuffers)  ID3D11Buffer *const *ppVertexBuffers,
		/* [annotation] */
		__in_ecount(NumBuffers)  const UINT *pStrides,
		/* [annotation] */
		__in_ecount(NumBuffers)  const UINT *pOffsets)
{
	FrameAnalysisLog("IASetVertexBuffers(StartSlot:%u, NumBuffers:%u, ppVertexBuffers:0x%p, pStrides:0x%p, pOffsets:0x%p)\n",
			StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppVertexBuffers);

	mOrigContext->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

STDMETHODIMP_(void) FrameAnalysisContext::GSSetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__in_ecount(NumBuffers) ID3D11Buffer *const *ppConstantBuffers)
{
	FrameAnalysisLog("GSSetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->GSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::GSSetShader(THIS_
		/* [annotation] */
		__in_opt ID3D11GeometryShader *pShader,
		/* [annotation] */
		__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances)
{
	mOrigContext->GSSetShader(pShader, ppClassInstances, NumClassInstances);

	FrameAnalysisLogNoNL("GSSetShader(pShader:0x%p, ppClassInstances:0x%p, NumClassInstances:%u)",
			pShader, ppClassInstances, NumClassInstances);
	FrameAnalysisLogShaderHash<ID3D11GeometryShader>(pShader);
}

STDMETHODIMP_(void) FrameAnalysisContext::IASetPrimitiveTopology(THIS_
		/* [annotation] */
		__in D3D11_PRIMITIVE_TOPOLOGY Topology)
{
	FrameAnalysisLog("IASetPrimitiveTopology(Topology:%u)\n",
			Topology);

	mOrigContext->IASetPrimitiveTopology(Topology);
}

STDMETHODIMP_(void) FrameAnalysisContext::VSSetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__in_ecount(NumSamplers) ID3D11SamplerState *const *ppSamplers)
{
	FrameAnalysisLog("VSSetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);

	mOrigContext->VSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::PSSetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__in_ecount(NumSamplers) ID3D11SamplerState *const *ppSamplers)
{
	FrameAnalysisLog("PSSetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);

	mOrigContext->PSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::Begin(THIS_
		/* [annotation] */
		__in  ID3D11Asynchronous *pAsync)
{
	FrameAnalysisLogNoNL("Begin(pAsync:0x%p)", pAsync);
	FrameAnalysisLogAsyncQuery(pAsync);

	mOrigContext->Begin(pAsync);
}

STDMETHODIMP_(void) FrameAnalysisContext::End(THIS_
		/* [annotation] */
		__in  ID3D11Asynchronous *pAsync)
{
	FrameAnalysisLogNoNL("End(pAsync:0x%p)", pAsync);
	FrameAnalysisLogAsyncQuery(pAsync);

	mOrigContext->End(pAsync);
}

STDMETHODIMP FrameAnalysisContext::GetData(THIS_
		/* [annotation] */
		__in  ID3D11Asynchronous *pAsync,
		/* [annotation] */
		__out_bcount_opt(DataSize)  void *pData,
		/* [annotation] */
		__in  UINT DataSize,
		/* [annotation] */
		__in  UINT GetDataFlags)
{
	HRESULT ret = mOrigContext->GetData(pAsync, pData, DataSize, GetDataFlags);

	FrameAnalysisLogNoNL("GetData(pAsync:0x%p, pData:0x%p, DataSize:%u, GetDataFlags:%u) = %u",
			pAsync, pData, DataSize, GetDataFlags, ret);
	FrameAnalysisLogAsyncQuery(pAsync);
	if (SUCCEEDED(ret))
		FrameAnalysisLogData(pData, DataSize);

	return ret;
}

STDMETHODIMP_(void) FrameAnalysisContext::SetPredication(THIS_
		/* [annotation] */
		__in_opt ID3D11Predicate *pPredicate,
		/* [annotation] */
		__in  BOOL PredicateValue)
{
	FrameAnalysisLogNoNL("SetPredication(pPredicate:0x%p, PredicateValue:%s)",
			pPredicate, PredicateValue ? "true" : "false");
	FrameAnalysisLogAsyncQuery(pPredicate);

	return mOrigContext->SetPredication(pPredicate, PredicateValue);
}

STDMETHODIMP_(void) FrameAnalysisContext::GSSetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__in_ecount(NumViews) ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	FrameAnalysisLog("GSSetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);

	mOrigContext->GSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::GSSetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__in_ecount(NumSamplers) ID3D11SamplerState *const *ppSamplers)
{
	FrameAnalysisLog("GSSetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);

	mOrigContext->GSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::OMSetBlendState(THIS_
		/* [annotation] */
		__in_opt  ID3D11BlendState *pBlendState,
		/* [annotation] */
		__in_opt  const FLOAT BlendFactor[4],
		/* [annotation] */
		__in  UINT SampleMask)
{
	FrameAnalysisLog("OMSetBlendState(pBlendState:0x%p, BlendFactor:0x%p, SampleMask:%u)\n",
			pBlendState, BlendFactor, SampleMask); // Beware dereferencing optional BlendFactor

	mOrigContext->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
}

STDMETHODIMP_(void) FrameAnalysisContext::OMSetDepthStencilState(THIS_
		/* [annotation] */
		__in_opt  ID3D11DepthStencilState *pDepthStencilState,
		/* [annotation] */
		__in  UINT StencilRef)
{
	FrameAnalysisLog("OMSetDepthStencilState(pDepthStencilState:0x%p, StencilRef:%u)\n",
			pDepthStencilState, StencilRef);

	mOrigContext->OMSetDepthStencilState(pDepthStencilState, StencilRef);
}

STDMETHODIMP_(void) FrameAnalysisContext::SOSetTargets(THIS_
		/* [annotation] */
		__in_range(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumBuffers,
		/* [annotation] */
		__in_ecount_opt(NumBuffers)  ID3D11Buffer *const *ppSOTargets,
		/* [annotation] */
		__in_ecount_opt(NumBuffers)  const UINT *pOffsets)
{
	FrameAnalysisLog("SOSetTargets(NumBuffers:%u, ppSOTargets:0x%p, pOffsets:0x%p)\n",
			NumBuffers, ppSOTargets, pOffsets);
	FrameAnalysisLogResourceArray(0, NumBuffers, (ID3D11Resource *const *)ppSOTargets);

	mOrigContext->SOSetTargets(NumBuffers, ppSOTargets, pOffsets);
}

STDMETHODIMP_(void) FrameAnalysisContext::Dispatch(THIS_
		/* [annotation] */
		__in  UINT ThreadGroupCountX,
		/* [annotation] */
		__in  UINT ThreadGroupCountY,
		/* [annotation] */
		__in  UINT ThreadGroupCountZ)
{
	mOrigContext->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);

	FrameAnalysisLog("Dispatch(ThreadGroupCountX:%u, ThreadGroupCountY:%u, ThreadGroupCountZ:%u)\n",
			ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

STDMETHODIMP_(void) FrameAnalysisContext::DispatchIndirect(THIS_
		/* [annotation] */
		__in  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		__in  UINT AlignedByteOffsetForArgs)
{
	mOrigContext->DispatchIndirect(pBufferForArgs, AlignedByteOffsetForArgs);

	FrameAnalysisLog("DispatchIndirect(pBufferForArgs:0x%p, AlignedByteOffsetForArgs:%u)\n",
			pBufferForArgs, AlignedByteOffsetForArgs);
}

STDMETHODIMP_(void) FrameAnalysisContext::RSSetState(THIS_
		/* [annotation] */
		__in_opt  ID3D11RasterizerState *pRasterizerState)
{
	FrameAnalysisLog("RSSetState(pRasterizerState:0x%p)\n",
			pRasterizerState);

	mOrigContext->RSSetState(pRasterizerState);
}

STDMETHODIMP_(void) FrameAnalysisContext::RSSetViewports(THIS_
		/* [annotation] */
		__in_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)  UINT NumViewports,
		/* [annotation] */
		__in_ecount_opt(NumViewports)  const D3D11_VIEWPORT *pViewports)
{
	FrameAnalysisLog("RSSetViewports(NumViewports:%u, pViewports:0x%p)\n",
			NumViewports, pViewports);

	mOrigContext->RSSetViewports(NumViewports, pViewports);
}

STDMETHODIMP_(void) FrameAnalysisContext::RSSetScissorRects(THIS_
		/* [annotation] */
		__in_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)  UINT NumRects,
		/* [annotation] */
		__in_ecount_opt(NumRects)  const D3D11_RECT *pRects)
{
	FrameAnalysisLog("RSSetScissorRects(NumRects:%u, pRects:0x%p)\n",
			NumRects, pRects);

	mOrigContext->RSSetScissorRects(NumRects, pRects);
}

STDMETHODIMP_(void) FrameAnalysisContext::CopySubresourceRegion(THIS_
		/* [annotation] */
		__in  ID3D11Resource *pDstResource,
		/* [annotation] */
		__in  UINT DstSubresource,
		/* [annotation] */
		__in  UINT DstX,
		/* [annotation] */
		__in  UINT DstY,
		/* [annotation] */
		__in  UINT DstZ,
		/* [annotation] */
		__in  ID3D11Resource *pSrcResource,
		/* [annotation] */
		__in  UINT SrcSubresource,
		/* [annotation] */
		__in_opt  const D3D11_BOX *pSrcBox)
{
	FrameAnalysisLog("CopySubresourceRegion(pDstResource:0x%p, DstSubresource:%u, DstX:%u, DstY:%u, DstZ:%u, pSrcResource:0x%p, SrcSubresource:%u, pSrcBox:0x%p)\n",
			pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
	FrameAnalysisLogResource(-1, "Src", pSrcResource);
	FrameAnalysisLogResource(-1, "Dst", pDstResource);

	mOrigContext->CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ,
			pSrcResource, SrcSubresource, pSrcBox);
}

STDMETHODIMP_(void) FrameAnalysisContext::CopyResource(THIS_
		/* [annotation] */
		__in  ID3D11Resource *pDstResource,
		/* [annotation] */
		__in  ID3D11Resource *pSrcResource)
{
	FrameAnalysisLog("CopyResource(pDstResource:0x%p, pSrcResource:0x%p)\n",
			pDstResource, pSrcResource);
	FrameAnalysisLogResource(-1, "Src", pSrcResource);
	FrameAnalysisLogResource(-1, "Dst", pDstResource);

	mOrigContext->CopyResource(pDstResource, pSrcResource);
}

STDMETHODIMP_(void) FrameAnalysisContext::UpdateSubresource(THIS_
		/* [annotation] */
		__in  ID3D11Resource *pDstResource,
		/* [annotation] */
		__in  UINT DstSubresource,
		/* [annotation] */
		__in_opt  const D3D11_BOX *pDstBox,
		/* [annotation] */
		__in  const void *pSrcData,
		/* [annotation] */
		__in  UINT SrcRowPitch,
		/* [annotation] */
		__in  UINT SrcDepthPitch)
{
	FrameAnalysisLogNoNL("UpdateSubresource(pDstResource:0x%p, DstSubresource:%u, pDstBox:0x%p, pSrcData:0x%p, SrcRowPitch:%u, SrcDepthPitch:%u)",
			pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
	FrameAnalysisLogResourceHash(pDstResource);

	mOrigContext->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch,
			SrcDepthPitch);
}

STDMETHODIMP_(void) FrameAnalysisContext::CopyStructureCount(THIS_
		/* [annotation] */
		__in  ID3D11Buffer *pDstBuffer,
		/* [annotation] */
		__in  UINT DstAlignedByteOffset,
		/* [annotation] */
		__in  ID3D11UnorderedAccessView *pSrcView)
{
	FrameAnalysisLog("CopyStructureCount(pDstBuffer:0x%p, DstAlignedByteOffset:%u, pSrcView:0x%p)\n",
			pDstBuffer, DstAlignedByteOffset, pSrcView);
	FrameAnalysisLogView(-1, "Src", (ID3D11View*)pSrcView);
	FrameAnalysisLogResource(-1, "Dst", pDstBuffer);

	mOrigContext->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView);
}

STDMETHODIMP_(void) FrameAnalysisContext::ClearUnorderedAccessViewUint(THIS_
		/* [annotation] */
		__in  ID3D11UnorderedAccessView *pUnorderedAccessView,
		/* [annotation] */
		__in  const UINT Values[4])
{
	FrameAnalysisLog("ClearUnorderedAccessViewUint(pUnorderedAccessView:0x%p, Values:0x%p\n)",
			pUnorderedAccessView, Values);
	FrameAnalysisLogView(-1, NULL, pUnorderedAccessView);

	mOrigContext->ClearUnorderedAccessViewUint(pUnorderedAccessView, Values);
}

STDMETHODIMP_(void) FrameAnalysisContext::ClearUnorderedAccessViewFloat(THIS_
		/* [annotation] */
		__in  ID3D11UnorderedAccessView *pUnorderedAccessView,
		/* [annotation] */
		__in  const FLOAT Values[4])
{
	FrameAnalysisLog("ClearUnorderedAccessViewFloat(pUnorderedAccessView:0x%p, Values:0x%p\n)",
			pUnorderedAccessView, Values);
	FrameAnalysisLogView(-1, NULL, pUnorderedAccessView);

	mOrigContext->ClearUnorderedAccessViewFloat(pUnorderedAccessView, Values);
}

STDMETHODIMP_(void) FrameAnalysisContext::ClearDepthStencilView(THIS_
		/* [annotation] */
		__in  ID3D11DepthStencilView *pDepthStencilView,
		/* [annotation] */
		__in  UINT ClearFlags,
		/* [annotation] */
		__in  FLOAT Depth,
		/* [annotation] */
		__in  UINT8 Stencil)
{
	FrameAnalysisLog("ClearDepthStencilView(pDepthStencilView:0x%p, ClearFlags:%u, Depth:%f, Stencil:%u\n)",
			pDepthStencilView, ClearFlags, Depth, Stencil);
	FrameAnalysisLogView(-1, NULL, pDepthStencilView);

	mOrigContext->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
}

STDMETHODIMP_(void) FrameAnalysisContext::GenerateMips(THIS_
		/* [annotation] */
		__in  ID3D11ShaderResourceView *pShaderResourceView)
{
	FrameAnalysisLog("GenerateMips(pShaderResourceView:0x%p\n)",
			pShaderResourceView);
	FrameAnalysisLogView(-1, NULL, pShaderResourceView);

	mOrigContext->GenerateMips(pShaderResourceView);
}

STDMETHODIMP_(void) FrameAnalysisContext::SetResourceMinLOD(THIS_
		/* [annotation] */
		__in  ID3D11Resource *pResource,
		FLOAT MinLOD)
{
	FrameAnalysisLogNoNL("SetResourceMinLOD(pResource:0x%p)",
			pResource);
	FrameAnalysisLogResourceHash(pResource);

	mOrigContext->SetResourceMinLOD(pResource, MinLOD);
}

STDMETHODIMP_(FLOAT) FrameAnalysisContext::GetResourceMinLOD(THIS_
		/* [annotation] */
		__in  ID3D11Resource *pResource)
{
	FLOAT ret = mOrigContext->GetResourceMinLOD(pResource);

	FrameAnalysisLogNoNL("GetResourceMinLOD(pResource:0x%p) = %f",
			pResource, ret);
	FrameAnalysisLogResourceHash(pResource);
	return ret;
}

STDMETHODIMP_(void) FrameAnalysisContext::ResolveSubresource(THIS_
		/* [annotation] */
		__in  ID3D11Resource *pDstResource,
		/* [annotation] */
		__in  UINT DstSubresource,
		/* [annotation] */
		__in  ID3D11Resource *pSrcResource,
		/* [annotation] */
		__in  UINT SrcSubresource,
		/* [annotation] */
		__in  DXGI_FORMAT Format)
{
	FrameAnalysisLog("ResolveSubresource(pDstResource:0x%p, DstSubresource:%u, pSrcResource:0x%p, SrcSubresource:%u, Format:%u)\n",
			pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
	FrameAnalysisLogResource(-1, "Src", pSrcResource);
	FrameAnalysisLogResource(-1, "Dst", pDstResource);

	mOrigContext->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
}

STDMETHODIMP_(void) FrameAnalysisContext::ExecuteCommandList(THIS_
		/* [annotation] */
		__in  ID3D11CommandList *pCommandList,
		BOOL RestoreContextState)
{
	FrameAnalysisLog("ExecuteCommandList(pCommandList:0x%p, RestoreContextState:%s)\n",
			pCommandList, RestoreContextState ? "true" : "false");

	mOrigContext->ExecuteCommandList(pCommandList, RestoreContextState);
}

STDMETHODIMP_(void) FrameAnalysisContext::HSSetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__in_ecount(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	FrameAnalysisLog("HSSetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);

	mOrigContext->HSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::HSSetShader(THIS_
		/* [annotation] */
		__in_opt  ID3D11HullShader *pHullShader,
		/* [annotation] */
		__in_ecount_opt(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances)
{
	mOrigContext->HSSetShader(pHullShader, ppClassInstances, NumClassInstances);

	FrameAnalysisLogNoNL("HSSetShader(pHullShader:0x%p, ppClassInstances:0x%p, NumClassInstances:%u)",
			pHullShader, ppClassInstances, NumClassInstances);
	FrameAnalysisLogShaderHash<ID3D11HullShader>(pHullShader);
}

STDMETHODIMP_(void) FrameAnalysisContext::HSSetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__in_ecount(NumSamplers)  ID3D11SamplerState *const *ppSamplers)
{
	FrameAnalysisLog("HSSetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);

	mOrigContext->HSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::HSSetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__in_ecount(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers)
{
	FrameAnalysisLog("HSSetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->HSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::DSSetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__in_ecount(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	FrameAnalysisLog("DSSetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);

	mOrigContext->DSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::DSSetShader(THIS_
		/* [annotation] */
		__in_opt  ID3D11DomainShader *pDomainShader,
		/* [annotation] */
		__in_ecount_opt(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances)
{
	mOrigContext->DSSetShader(pDomainShader, ppClassInstances, NumClassInstances);

	FrameAnalysisLogNoNL("DSSetShader(pDomainShader:0x%p, ppClassInstances:0x%p, NumClassInstances:%u)",
			pDomainShader, ppClassInstances, NumClassInstances);
	FrameAnalysisLogShaderHash<ID3D11DomainShader>(pDomainShader);
}

STDMETHODIMP_(void) FrameAnalysisContext::DSSetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__in_ecount(NumSamplers)  ID3D11SamplerState *const *ppSamplers)
{
	FrameAnalysisLog("DSSetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);

	mOrigContext->DSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::DSSetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__in_ecount(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers)
{
	FrameAnalysisLog("DSSetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->DSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSSetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__in_ecount(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	FrameAnalysisLog("CSSetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);

	mOrigContext->CSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSSetUnorderedAccessViews(THIS_
		/* [annotation] */
		__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - StartSlot)  UINT NumUAVs,
		/* [annotation] */
		__in_ecount(NumUAVs)  ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
		/* [annotation] */
		__in_ecount(NumUAVs)  const UINT *pUAVInitialCounts)
{
	FrameAnalysisLog("CSSetUnorderedAccessViews(StartSlot:%u, NumUAVs:%u, ppUnorderedAccessViews:0x%p, pUAVInitialCounts:0x%p)\n",
			StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
	FrameAnalysisLogViewArray(StartSlot, NumUAVs, (ID3D11View *const *)ppUnorderedAccessViews);

	mOrigContext->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSSetShader(THIS_
		/* [annotation] */
		__in_opt  ID3D11ComputeShader *pComputeShader,
		/* [annotation] */
		__in_ecount_opt(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances)
{
	mOrigContext->CSSetShader(pComputeShader, ppClassInstances, NumClassInstances);

	FrameAnalysisLogNoNL("CSSetShader(pComputeShader:0x%p, ppClassInstances:0x%p, NumClassInstances:%u)",
			pComputeShader, ppClassInstances, NumClassInstances);
	FrameAnalysisLogShaderHash<ID3D11ComputeShader>(pComputeShader);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSSetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__in_ecount(NumSamplers)  ID3D11SamplerState *const *ppSamplers)
{
	FrameAnalysisLog("CSSetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);

	mOrigContext->CSSetSamplers(StartSlot, NumSamplers, ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSSetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__in_ecount(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers)
{
	FrameAnalysisLog("CSSetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->CSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::VSGetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	mOrigContext->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);

	FrameAnalysisLog("VSGetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::PSGetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	mOrigContext->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);

	FrameAnalysisLog("PSGetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::PSGetShader(THIS_
		/* [annotation] */
		__out  ID3D11PixelShader **ppPixelShader,
		/* [annotation] */
		__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		__inout_opt  UINT *pNumClassInstances)
{
	mOrigContext->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances);

	FrameAnalysisLogNoNL("PSGetShader(ppPixelShader:0x%p, ppClassInstances:0x%p, pNumClassInstances:0x%p)",
			ppPixelShader, ppClassInstances, pNumClassInstances);
	if (ppPixelShader)
		FrameAnalysisLogShaderHash<ID3D11PixelShader>(*ppPixelShader);
	else
		FrameAnalysisLog("\n");
}

STDMETHODIMP_(void) FrameAnalysisContext::PSGetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	mOrigContext->PSGetSamplers(StartSlot, NumSamplers, ppSamplers);

	FrameAnalysisLog("PSGetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::VSGetShader(THIS_
		/* [annotation] */
		__out  ID3D11VertexShader **ppVertexShader,
		/* [annotation] */
		__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		__inout_opt  UINT *pNumClassInstances)
{
	mOrigContext->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances);

	FrameAnalysisLogNoNL("VSGetShader(ppVertexShader:0x%p, ppClassInstances:0x%p, pNumClassInstances:0x%p)",
			ppVertexShader, ppClassInstances, pNumClassInstances);
	if (ppVertexShader)
		FrameAnalysisLogShaderHash<ID3D11VertexShader>(*ppVertexShader);
	else
		FrameAnalysisLog("\n");
}

STDMETHODIMP_(void) FrameAnalysisContext::PSGetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	mOrigContext->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);

	FrameAnalysisLog("PSGetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::IAGetInputLayout(THIS_
		/* [annotation] */
		__out  ID3D11InputLayout **ppInputLayout)
{
	mOrigContext->IAGetInputLayout(ppInputLayout);

	FrameAnalysisLog("IAGetInputLayout(ppInputLayout:0x%p)\n",
			ppInputLayout);
}

STDMETHODIMP_(void) FrameAnalysisContext::IAGetVertexBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__out_ecount_opt(NumBuffers)  ID3D11Buffer **ppVertexBuffers,
		/* [annotation] */
		__out_ecount_opt(NumBuffers)  UINT *pStrides,
		/* [annotation] */
		__out_ecount_opt(NumBuffers)  UINT *pOffsets)
{
	mOrigContext->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);

	FrameAnalysisLog("IAGetVertexBuffers(StartSlot:%u, NumBuffers:%u, ppVertexBuffers:0x%p, pStrides:0x%p, pOffsets:0x%p)\n",
			StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppVertexBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::IAGetIndexBuffer(THIS_
		/* [annotation] */
		__out_opt  ID3D11Buffer **pIndexBuffer,
		/* [annotation] */
		__out_opt  DXGI_FORMAT *Format,
		/* [annotation] */
		__out_opt  UINT *Offset)
{
	mOrigContext->IAGetIndexBuffer(pIndexBuffer, Format, Offset);

	FrameAnalysisLog("IAGetIndexBuffer(pIndexBuffer:0x%p, Format:0x%p, Offset:0x%p)\n",
			pIndexBuffer, Format, Offset);
}

STDMETHODIMP_(void) FrameAnalysisContext::GSGetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	mOrigContext->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);

	FrameAnalysisLog("GSGetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::GSGetShader(THIS_
		/* [annotation] */
		__out  ID3D11GeometryShader **ppGeometryShader,
		/* [annotation] */
		__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		__inout_opt  UINT *pNumClassInstances)
{
	mOrigContext->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances);

	FrameAnalysisLogNoNL("GSGetShader(ppGeometryShader:0x%p, ppClassInstances:0x%p, pNumClassInstances:0x%p)",
			ppGeometryShader, ppClassInstances, pNumClassInstances);
	if (ppGeometryShader)
		FrameAnalysisLogShaderHash<ID3D11GeometryShader>(*ppGeometryShader);
	else
		FrameAnalysisLog("\n");
}

STDMETHODIMP_(void) FrameAnalysisContext::IAGetPrimitiveTopology(THIS_
		/* [annotation] */
		__out  D3D11_PRIMITIVE_TOPOLOGY *pTopology)
{
	mOrigContext->IAGetPrimitiveTopology(pTopology);

	FrameAnalysisLog("IAGetPrimitiveTopology(pTopology:0x%p)\n",
			pTopology);
}

STDMETHODIMP_(void) FrameAnalysisContext::VSGetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	mOrigContext->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);

	FrameAnalysisLog("VSGetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::VSGetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	mOrigContext->VSGetSamplers(StartSlot, NumSamplers, ppSamplers);

	FrameAnalysisLog("VSGetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::GetPredication(THIS_
		/* [annotation] */
		__out_opt  ID3D11Predicate **ppPredicate,
		/* [annotation] */
		__out_opt  BOOL *pPredicateValue)
{
	mOrigContext->GetPredication(ppPredicate, pPredicateValue);

	FrameAnalysisLogNoNL("GetPredication(ppPredicate:0x%p, pPredicateValue:0x%p)",
			ppPredicate, pPredicateValue);
	FrameAnalysisLogAsyncQuery(ppPredicate ? *ppPredicate : NULL);
}

STDMETHODIMP_(void) FrameAnalysisContext::GSGetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	mOrigContext->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);

	FrameAnalysisLog("GSGetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::GSGetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	mOrigContext->GSGetSamplers(StartSlot, NumSamplers, ppSamplers);

	FrameAnalysisLog("GSGetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::OMGetRenderTargets(THIS_
		/* [annotation] */
		__in_range(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumViews,
		/* [annotation] */
		__out_ecount_opt(NumViews)  ID3D11RenderTargetView **ppRenderTargetViews,
		/* [annotation] */
		__out_opt  ID3D11DepthStencilView **ppDepthStencilView)
{
	mOrigContext->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView);

	FrameAnalysisLog("OMGetRenderTargets(NumViews:%u, ppRenderTargetViews:0x%p, ppDepthStencilView:0x%p)\n",
			NumViews, ppRenderTargetViews, ppDepthStencilView);
	FrameAnalysisLogViewArray(0, NumViews, (ID3D11View *const *)ppRenderTargetViews);
	if (ppDepthStencilView)
		FrameAnalysisLogView(-1, "D", *ppDepthStencilView);
}

STDMETHODIMP_(void) FrameAnalysisContext::OMGetRenderTargetsAndUnorderedAccessViews(THIS_
		/* [annotation] */
		__in_range(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumRTVs,
		/* [annotation] */
		__out_ecount_opt(NumRTVs)  ID3D11RenderTargetView **ppRenderTargetViews,
		/* [annotation] */
		__out_opt  ID3D11DepthStencilView **ppDepthStencilView,
		/* [annotation] */
		__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT UAVStartSlot,
		/* [annotation] */
		__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - UAVStartSlot)  UINT NumUAVs,
		/* [annotation] */
		__out_ecount_opt(NumUAVs)  ID3D11UnorderedAccessView **ppUnorderedAccessViews)
{
	mOrigContext->OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, ppDepthStencilView,
			UAVStartSlot, NumUAVs, ppUnorderedAccessViews);

	FrameAnalysisLog("OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs:%i, ppRenderTargetViews:0x%p, ppDepthStencilView:0x%p, UAVStartSlot:%i, NumUAVs:%u, ppUnorderedAccessViews:0x%p)\n",
			NumRTVs, ppRenderTargetViews, ppDepthStencilView,
			UAVStartSlot, NumUAVs, ppUnorderedAccessViews);
	FrameAnalysisLogViewArray(0, NumRTVs, (ID3D11View *const *)ppRenderTargetViews);
	if (ppDepthStencilView)
		FrameAnalysisLogView(-1, "D", *ppDepthStencilView);
	FrameAnalysisLogViewArray(UAVStartSlot, NumUAVs, (ID3D11View *const *)ppUnorderedAccessViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::OMGetBlendState(THIS_
		/* [annotation] */
		__out_opt  ID3D11BlendState **ppBlendState,
		/* [annotation] */
		__out_opt  FLOAT BlendFactor[4],
		/* [annotation] */
		__out_opt  UINT *pSampleMask)
{
	mOrigContext->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask);

	FrameAnalysisLog("OMGetBlendState(ppBlendState:0x%p, BlendFactor:0x%p, pSampleMask:0x%p)\n",
			ppBlendState, BlendFactor, pSampleMask);
}

STDMETHODIMP_(void) FrameAnalysisContext::OMGetDepthStencilState(THIS_
		/* [annotation] */
		__out_opt  ID3D11DepthStencilState **ppDepthStencilState,
		/* [annotation] */
		__out_opt  UINT *pStencilRef)
{
	mOrigContext->OMGetDepthStencilState(ppDepthStencilState, pStencilRef);

	FrameAnalysisLog("OMGetDepthStencilState(ppDepthStencilState:0x%p, pStencilRef:0x%p)\n",
			ppDepthStencilState, pStencilRef);
}

STDMETHODIMP_(void) FrameAnalysisContext::SOGetTargets(THIS_
		/* [annotation] */
		__in_range(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumBuffers,
		/* [annotation] */
		__out_ecount(NumBuffers)  ID3D11Buffer **ppSOTargets)
{
	mOrigContext->SOGetTargets(NumBuffers, ppSOTargets);

	FrameAnalysisLog("SOGetTargets(NumBuffers:%u, ppSOTargets:0x%p)\n",
			NumBuffers, ppSOTargets);
	FrameAnalysisLogResourceArray(0, NumBuffers, (ID3D11Resource *const *)ppSOTargets);
}

STDMETHODIMP_(void) FrameAnalysisContext::RSGetState(THIS_
		/* [annotation] */
		__out  ID3D11RasterizerState **ppRasterizerState)
{
	mOrigContext->RSGetState(ppRasterizerState);

	FrameAnalysisLog("RSGetState(ppRasterizerState:0x%p)\n",
			ppRasterizerState);
}

STDMETHODIMP_(void) FrameAnalysisContext::RSGetViewports(THIS_
		/* [annotation] */
		__inout /*_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE )*/   UINT *pNumViewports,
		/* [annotation] */
		__out_ecount_opt(*pNumViewports)  D3D11_VIEWPORT *pViewports)
{
	mOrigContext->RSGetViewports(pNumViewports, pViewports);

	FrameAnalysisLog("RSGetViewports(pNumViewports:0x%p, pViewports:0x%p)\n",
			pNumViewports, pViewports);
}

STDMETHODIMP_(void) FrameAnalysisContext::RSGetScissorRects(THIS_
		/* [annotation] */
		__inout /*_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE )*/   UINT *pNumRects,
		/* [annotation] */
		__out_ecount_opt(*pNumRects)  D3D11_RECT *pRects)
{
	mOrigContext->RSGetScissorRects(pNumRects, pRects);

	FrameAnalysisLog("RSGetScissorRects(pNumRects:0x%p, pRects:0x%p)\n",
			pNumRects, pRects);
}

STDMETHODIMP_(void) FrameAnalysisContext::HSGetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	mOrigContext->HSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);

	FrameAnalysisLog("HSGetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::HSGetShader(THIS_
		/* [annotation] */
		__out  ID3D11HullShader **ppHullShader,
		/* [annotation] */
		__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		__inout_opt  UINT *pNumClassInstances)
{
	mOrigContext->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances);

	FrameAnalysisLogNoNL("HSGetShader(ppHullShader:0x%p, ppClassInstances:0x%p, pNumClassInstances:0x%p)",
			ppHullShader, ppClassInstances, pNumClassInstances);
	if (ppHullShader)
		FrameAnalysisLogShaderHash<ID3D11HullShader>(*ppHullShader);
	else
		FrameAnalysisLog("\n");
}

STDMETHODIMP_(void) FrameAnalysisContext::HSGetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	mOrigContext->HSGetSamplers(StartSlot, NumSamplers, ppSamplers);

	FrameAnalysisLog("HSGetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::HSGetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	mOrigContext->HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);

	FrameAnalysisLog("HSGetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::DSGetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	mOrigContext->DSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);

	FrameAnalysisLog("DSGetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::DSGetShader(THIS_
		/* [annotation] */
		__out  ID3D11DomainShader **ppDomainShader,
		/* [annotation] */
		__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		__inout_opt  UINT *pNumClassInstances)
{
	mOrigContext->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances);

	FrameAnalysisLogNoNL("DSGetShader(ppDomainShader:0x%p, ppClassInstances:0x%p, pNumClassInstances:0x%p)",
			ppDomainShader, ppClassInstances, pNumClassInstances);
	if (ppDomainShader)
		FrameAnalysisLogShaderHash<ID3D11DomainShader>(*ppDomainShader);
	else
		FrameAnalysisLog("\n");
}

STDMETHODIMP_(void) FrameAnalysisContext::DSGetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	mOrigContext->DSGetSamplers(StartSlot, NumSamplers, ppSamplers);

	FrameAnalysisLog("DSGetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::DSGetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	mOrigContext->DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);

	FrameAnalysisLog("DSGetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSGetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__out_ecount(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews)
{
	mOrigContext->CSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);

	FrameAnalysisLog("CSGetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSGetUnorderedAccessViews(THIS_
		/* [annotation] */
		__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - StartSlot)  UINT NumUAVs,
		/* [annotation] */
		__out_ecount(NumUAVs)  ID3D11UnorderedAccessView **ppUnorderedAccessViews)
{
	mOrigContext->CSGetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews);

	FrameAnalysisLog("CSGetUnorderedAccessViews(StartSlot:%u, NumUAVs:%u, ppUnorderedAccessViews:0x%p)\n",
			StartSlot, NumUAVs, ppUnorderedAccessViews);
	FrameAnalysisLogViewArray(StartSlot, NumUAVs, (ID3D11View *const *)ppUnorderedAccessViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSGetShader(THIS_
		/* [annotation] */
		__out  ID3D11ComputeShader **ppComputeShader,
		/* [annotation] */
		__out_ecount_opt(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		__inout_opt  UINT *pNumClassInstances)
{
	mOrigContext->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances);

	FrameAnalysisLogNoNL("CSGetShader(ppComputeShader:0x%p, ppClassInstances:0x%p, pNumClassInstances:0x%p)",
			ppComputeShader, ppClassInstances, pNumClassInstances);
	if (ppComputeShader)
		FrameAnalysisLogShaderHash<ID3D11ComputeShader>(*ppComputeShader);
	else
		FrameAnalysisLog("\n");
}

STDMETHODIMP_(void) FrameAnalysisContext::CSGetSamplers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		__out_ecount(NumSamplers)  ID3D11SamplerState **ppSamplers)
{
	mOrigContext->CSGetSamplers(StartSlot, NumSamplers, ppSamplers);

	FrameAnalysisLog("CSGetSamplers(StartSlot:%u, NumSamplers:%u, ppSamplers:0x%p)\n",
			StartSlot, NumSamplers, ppSamplers);
	FrameAnalysisLogMiscArray(StartSlot, NumSamplers, (void *const *)ppSamplers);
}

STDMETHODIMP_(void) FrameAnalysisContext::CSGetConstantBuffers(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		__out_ecount(NumBuffers)  ID3D11Buffer **ppConstantBuffers)
{
	mOrigContext->CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);

	FrameAnalysisLog("CSGetConstantBuffers(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

STDMETHODIMP_(void) FrameAnalysisContext::ClearState(THIS)
{
	FrameAnalysisLog("ClearState()\n");

	mOrigContext->ClearState();
}

STDMETHODIMP_(void) FrameAnalysisContext::Flush(THIS)
{
	FrameAnalysisLog("Flush()\n");

	mOrigContext->Flush();
}

STDMETHODIMP_(D3D11_DEVICE_CONTEXT_TYPE) FrameAnalysisContext::GetType(THIS)
{
	D3D11_DEVICE_CONTEXT_TYPE ret = mOrigContext->GetType();

	FrameAnalysisLog("GetType() = %u\n", ret);
	return ret;
}

STDMETHODIMP_(UINT) FrameAnalysisContext::GetContextFlags(THIS)
{
	UINT ret = mOrigContext->GetContextFlags();

	FrameAnalysisLog("GetContextFlags() = %u\n", ret);
	return ret;
}

STDMETHODIMP FrameAnalysisContext::FinishCommandList(THIS_
		BOOL RestoreDeferredContextState,
		/* [annotation] */
		__out_opt  ID3D11CommandList **ppCommandList)
{
	HRESULT ret = mOrigContext->FinishCommandList(RestoreDeferredContextState, ppCommandList);

	FrameAnalysisLog("FinishCommandList(ppCommandList:0x%p -> 0x%p) = %u\n", ppCommandList, ppCommandList ? *ppCommandList : NULL, ret);
	return ret;
}

STDMETHODIMP_(void) FrameAnalysisContext::VSSetShader(THIS_
		/* [annotation] */
		__in_opt ID3D11VertexShader *pVertexShader,
		/* [annotation] */
		__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances)
{
	mOrigContext->VSSetShader(pVertexShader, ppClassInstances, NumClassInstances);

	FrameAnalysisLogNoNL("VSSetShader(pVertexShader:0x%p, ppClassInstances:0x%p, NumClassInstances:%u)",
			pVertexShader, ppClassInstances, NumClassInstances);
	FrameAnalysisLogShaderHash<ID3D11VertexShader>(pVertexShader);
}

STDMETHODIMP_(void) FrameAnalysisContext::PSSetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__in_ecount(NumViews) ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	FrameAnalysisLog("PSSetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);

	mOrigContext->PSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::PSSetShader(THIS_
		/* [annotation] */
		__in_opt ID3D11PixelShader *pPixelShader,
		/* [annotation] */
		__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances)
{
	mOrigContext->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances);

	FrameAnalysisLogNoNL("PSSetShader(pPixelShader:0x%p, ppClassInstances:0x%p, NumClassInstances:%u)",
			pPixelShader, ppClassInstances, NumClassInstances);
	FrameAnalysisLogShaderHash<ID3D11PixelShader>(pPixelShader);
}

STDMETHODIMP_(void) FrameAnalysisContext::DrawIndexed(THIS_
		/* [annotation] */
		__in  UINT IndexCount,
		/* [annotation] */
		__in  UINT StartIndexLocation,
		/* [annotation] */
		__in  INT BaseVertexLocation)
{
	FrameAnalysisLog("DrawIndexed(IndexCount:%u, StartIndexLocation:%u, BaseVertexLocation:%u)\n",
			IndexCount, StartIndexLocation, BaseVertexLocation);

	mOrigContext->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
}

STDMETHODIMP_(void) FrameAnalysisContext::Draw(THIS_
		/* [annotation] */
		__in  UINT VertexCount,
		/* [annotation] */
		__in  UINT StartVertexLocation)
{
	FrameAnalysisLog("Draw(VertexCount:%u, StartVertexLocation:%u)\n",
			VertexCount, StartVertexLocation);

	mOrigContext->Draw(VertexCount, StartVertexLocation);
}

STDMETHODIMP_(void) FrameAnalysisContext::IASetIndexBuffer(THIS_
		/* [annotation] */
		__in_opt ID3D11Buffer *pIndexBuffer,
		/* [annotation] */
		__in DXGI_FORMAT Format,
		/* [annotation] */
		__in  UINT Offset)
{
	FrameAnalysisLogNoNL("IASetIndexBuffer(pIndexBuffer:0x%p, Format:%u, Offset:%u)",
			pIndexBuffer, Format, Offset);
	FrameAnalysisLogResourceHash(pIndexBuffer);

	mOrigContext->IASetIndexBuffer(pIndexBuffer, Format, Offset);
}

STDMETHODIMP_(void) FrameAnalysisContext::DrawIndexedInstanced(THIS_
		/* [annotation] */
		__in  UINT IndexCountPerInstance,
		/* [annotation] */
		__in  UINT InstanceCount,
		/* [annotation] */
		__in  UINT StartIndexLocation,
		/* [annotation] */
		__in  INT BaseVertexLocation,
		/* [annotation] */
		__in  UINT StartInstanceLocation)
{
	FrameAnalysisLog("DrawIndexedInstanced(IndexCountPerInstance:%u, InstanceCount:%u, StartIndexLocation:%u, BaseVertexLocation:%i, StartInstanceLocation:%u)\n",
			IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);

	mOrigContext->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation,
			BaseVertexLocation, StartInstanceLocation);
}

STDMETHODIMP_(void) FrameAnalysisContext::DrawInstanced(THIS_
		/* [annotation] */
		__in  UINT VertexCountPerInstance,
		/* [annotation] */
		__in  UINT InstanceCount,
		/* [annotation] */
		__in  UINT StartVertexLocation,
		/* [annotation] */
		__in  UINT StartInstanceLocation)
{
	FrameAnalysisLog("DrawInstanced(VertexCountPerInstance:%u, InstanceCount:%u, StartVertexLocation:%u, StartInstanceLocation:%u)\n",
			VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);

	mOrigContext->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

STDMETHODIMP_(void) FrameAnalysisContext::VSSetShaderResources(THIS_
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		__in_range(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		__in_ecount(NumViews) ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
	FrameAnalysisLog("VSSetShaderResources(StartSlot:%u, NumViews:%u, ppShaderResourceViews:0x%p)\n",
			StartSlot, NumViews, ppShaderResourceViews);
	FrameAnalysisLogViewArray(StartSlot, NumViews, (ID3D11View *const *)ppShaderResourceViews);

	mOrigContext->VSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

STDMETHODIMP_(void) FrameAnalysisContext::OMSetRenderTargets(THIS_
		/* [annotation] */
		__in_range(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumViews,
		/* [annotation] */
		__in_ecount_opt(NumViews) ID3D11RenderTargetView *const *ppRenderTargetViews,
		/* [annotation] */
		__in_opt ID3D11DepthStencilView *pDepthStencilView)
{
	FrameAnalysisLog("OMSetRenderTargets(NumViews:%u, ppRenderTargetViews:0x%p, pDepthStencilView:0x%p)\n",
			NumViews, ppRenderTargetViews, pDepthStencilView);
	FrameAnalysisLogViewArray(0, NumViews, (ID3D11View *const *)ppRenderTargetViews);
	FrameAnalysisLogView(-1, "D", pDepthStencilView);

	mOrigContext->OMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);
}

STDMETHODIMP_(void) FrameAnalysisContext::OMSetRenderTargetsAndUnorderedAccessViews(THIS_
		/* [annotation] */
		__in  UINT NumRTVs,
		/* [annotation] */
		__in_ecount_opt(NumRTVs) ID3D11RenderTargetView *const *ppRenderTargetViews,
		/* [annotation] */
		__in_opt ID3D11DepthStencilView *pDepthStencilView,
		/* [annotation] */
		__in_range(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT UAVStartSlot,
		/* [annotation] */
		__in  UINT NumUAVs,
		/* [annotation] */
		__in_ecount_opt(NumUAVs) ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
		/* [annotation] */
		__in_ecount_opt(NumUAVs)  const UINT *pUAVInitialCounts)
{
	FrameAnalysisLog("OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs:%i, ppRenderTargetViews:0x%p, pDepthStencilView:0x%p, UAVStartSlot:%i, NumUAVs:%u, ppUnorderedAccessViews:0x%p, pUAVInitialCounts:0x%p)\n",
			NumRTVs, ppRenderTargetViews, pDepthStencilView,
			UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
			pUAVInitialCounts);
	FrameAnalysisLogViewArray(0, NumRTVs, (ID3D11View *const *)ppRenderTargetViews);
	FrameAnalysisLogView(-1, "D", pDepthStencilView);
	FrameAnalysisLogViewArray(UAVStartSlot, NumUAVs, (ID3D11View *const *)ppUnorderedAccessViews);

	mOrigContext->OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, pDepthStencilView,
			UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}

STDMETHODIMP_(void) FrameAnalysisContext::DrawAuto(THIS)
{
	FrameAnalysisLog("DrawAuto()\n");

	mOrigContext->DrawAuto();
}

STDMETHODIMP_(void) FrameAnalysisContext::DrawIndexedInstancedIndirect(THIS_
		/* [annotation] */
		__in  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		__in  UINT AlignedByteOffsetForArgs)
{
	FrameAnalysisLog("DrawIndexedInstancedIndirect(pBufferForArgs:0x%p, AlignedByteOffsetForArgs:%u)\n",
			pBufferForArgs, AlignedByteOffsetForArgs);

	mOrigContext->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
}

STDMETHODIMP_(void) FrameAnalysisContext::DrawInstancedIndirect(THIS_
		/* [annotation] */
		__in  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		__in  UINT AlignedByteOffsetForArgs)
{
	FrameAnalysisLog("DrawInstancedIndirect(pBufferForArgs:0x%p, AlignedByteOffsetForArgs:%u)\n",
			pBufferForArgs, AlignedByteOffsetForArgs);

	mOrigContext->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
}

STDMETHODIMP_(void) FrameAnalysisContext::ClearRenderTargetView(THIS_
		/* [annotation] */
		__in  ID3D11RenderTargetView *pRenderTargetView,
		/* [annotation] */
		__in  const FLOAT ColorRGBA[4])
{
	FrameAnalysisLog("ClearRenderTargetView(pRenderTargetView:0x%p, ColorRGBA:0x%p)\n",
			pRenderTargetView, ColorRGBA);
	FrameAnalysisLogView(-1, NULL, pRenderTargetView);

	mOrigContext->ClearRenderTargetView(pRenderTargetView, ColorRGBA);
}

void STDMETHODCALLTYPE FrameAnalysisContext::CopySubresourceRegion1(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_  UINT DstX,
		/* [annotation] */
		_In_  UINT DstY,
		/* [annotation] */
		_In_  UINT DstZ,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource,
		/* [annotation] */
		_In_  UINT SrcSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pSrcBox,
		/* [annotation] */
		_In_  UINT CopyFlags)
{
	FrameAnalysisLog("CopySubresourceRegion1(pDstResource:0x%p, DstSubresource:%u, DstX:%u, DstY:%u, DstZ:%u, pSrcResource:0x%p, SrcSubresource:%u, pSrcBox:0x%p, CopyFlags:%u)\n",
			pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox, CopyFlags);
	FrameAnalysisLogResource(-1, "Src", pSrcResource);
	FrameAnalysisLogResource(-1, "Dst", pDstResource);

	mOrigContext->CopySubresourceRegion1(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox, CopyFlags);
}

void STDMETHODCALLTYPE FrameAnalysisContext::UpdateSubresource1(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pDstBox,
		/* [annotation] */
		_In_  const void *pSrcData,
		/* [annotation] */
		_In_  UINT SrcRowPitch,
		/* [annotation] */
		_In_  UINT SrcDepthPitch,
		/* [annotation] */
		_In_  UINT CopyFlags)
{
	FrameAnalysisLogNoNL("UpdateSubresource1(pDstResource:0x%p, DstSubresource:%u, pDstBox:0x%p, pSrcData:0x%p, SrcRowPitch:%u, SrcDepthPitch:%u, CopyFlags:%u)",
			pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, CopyFlags);
	FrameAnalysisLogResourceHash(pDstResource);

	mOrigContext->UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, CopyFlags);
}

void STDMETHODCALLTYPE FrameAnalysisContext::DiscardResource(
		/* [annotation] */
		_In_  ID3D11Resource *pResource)
{
	FrameAnalysisLogNoNL("DiscardResource(pResource:0x%p)",
			pResource);
	FrameAnalysisLogResourceHash(pResource);

	mOrigContext->DiscardResource(pResource);
}

void STDMETHODCALLTYPE FrameAnalysisContext::DiscardView(
		/* [annotation] */
		_In_  ID3D11View *pResourceView)
{
	FrameAnalysisLog("DiscardView(pResourceView:0x%p)\n",
			pResourceView);
	FrameAnalysisLogView(-1, NULL, pResourceView);

	mOrigContext->DiscardView(pResourceView);
}

void STDMETHODCALLTYPE FrameAnalysisContext::VSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	FrameAnalysisLog("VSSetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->VSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE FrameAnalysisContext::HSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	FrameAnalysisLog("HSSetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->HSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE FrameAnalysisContext::DSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	FrameAnalysisLog("DSSetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->DSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE FrameAnalysisContext::GSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	FrameAnalysisLog("GSSetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->GSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE FrameAnalysisContext::PSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	FrameAnalysisLog("PSSetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->PSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE FrameAnalysisContext::CSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants)
{
	FrameAnalysisLog("CSSetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);

	mOrigContext->CSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE FrameAnalysisContext::VSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext->VSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);

	FrameAnalysisLog("VSGetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

void STDMETHODCALLTYPE FrameAnalysisContext::HSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext->HSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);

	FrameAnalysisLog("HSGetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

void STDMETHODCALLTYPE FrameAnalysisContext::DSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext->DSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);

	FrameAnalysisLog("DSGetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

void STDMETHODCALLTYPE FrameAnalysisContext::GSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext->GSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);

	FrameAnalysisLog("GSGetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

void STDMETHODCALLTYPE FrameAnalysisContext::PSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext->PSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);

	FrameAnalysisLog("PSGetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

void STDMETHODCALLTYPE FrameAnalysisContext::CSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants)
{
	mOrigContext->CSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);

	FrameAnalysisLog("CSGetConstantBuffers1(StartSlot:%u, NumBuffers:%u, ppConstantBuffers:0x%p, pFirstConstant:0x%p, pNumConstants:0x%p)\n",
			StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
	FrameAnalysisLogResourceArray(StartSlot, NumBuffers, (ID3D11Resource *const *)ppConstantBuffers);
}

void STDMETHODCALLTYPE FrameAnalysisContext::SwapDeviceContextState(
		/* [annotation] */
		_In_  ID3DDeviceContextState *pState,
		/* [annotation] */
		_Out_opt_  ID3DDeviceContextState **ppPreviousState)
{
	FrameAnalysisLog("SwapDeviceContextState(pState:0x%p, ppPreviousState:0x%p)\n",
			pState, ppPreviousState);

	mOrigContext->SwapDeviceContextState(pState, ppPreviousState);
}

void STDMETHODCALLTYPE FrameAnalysisContext::ClearView(
		/* [annotation] */
		_In_  ID3D11View *pView,
		/* [annotation] */
		_In_  const FLOAT Color[4],
		/* [annotation] */
		_In_reads_opt_(NumRects)  const D3D11_RECT *pRect,
		UINT NumRects)
{
	FrameAnalysisLog("ClearView(pView:0x%p, Color:0x%p, pRect:0x%p)\n",
			pView, Color, pRect);
	FrameAnalysisLogView(-1, NULL, pView);

	mOrigContext->ClearView(pView, Color, pRect, NumRects);
}

void STDMETHODCALLTYPE FrameAnalysisContext::DiscardView1(
		/* [annotation] */
		_In_  ID3D11View *pResourceView,
		/* [annotation] */
		_In_reads_opt_(NumRects)  const D3D11_RECT *pRects,
		UINT NumRects)
{
	FrameAnalysisLog("DiscardView1(pResourceView:0x%p, pRects:0x%p)\n",
			pResourceView, pRects);
	FrameAnalysisLogView(-1, NULL, pResourceView);

	mOrigContext->DiscardView1(pResourceView, pRects, NumRects);
}
