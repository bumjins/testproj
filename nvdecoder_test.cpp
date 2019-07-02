#include "stdafx.h"
#include "IDecoder.h"
#include "CommonInfo.h"
#include "Stopwatch.hpp"

#include <cuda.h>
#include <iostream>
#include "NvDecoder.h"
#include "NVDecUtils/NvCodecUtils.h"
#include "NvidiaDecoder.h"
#include "StringTools.h"
#include "FramePresenterD3D11.hpp"

NvidiaDecoder::NvidiaDecoder(DecoderInfo info) :
	m_pFmtCtx(NULL),
	m_pVCtx(NULL),
	m_nVSI(-1),
	m_swsContext(NULL),
	m_UseVIR(0),
	m_UseHWaccel(0),
	m_CurDecodeLevel(1)
{
	m_strUrl = info.url;
	m_wstrUrl.assign(m_strUrl.begin(), m_strUrl.end());
	CCommonInfo::GetInstance()->KSNCOutputDebugString(L"NvidiaDecoder Initialize - %s\n", m_wstrUrl.c_str());
	m_sourceType = CCommonInfo::GetInstance()->GetProtocol(m_strUrl.c_str());
	m_UseVIR = CCommonInfo::GetInstance()->ReadIniFile(L"MEDIACENTER", L"USEVIR", 1);
	m_UseHWaccel = CCommonInfo::GetInstance()->ReadIniFile(L"MEDIACENTER", L"USEHWACCEL", 0);
	m_swsAlgorithm = CCommonInfo::GetInstance()->ReadIniFile(L"MEDIACENTER", L"VIRALGORITHM", 16);
	m_ConnectionMode = info.ConnectionMode;
	m_DecodeLevel = -1;
	m_changeLevel = false;
};

NvidiaDecoder::~NvidiaDecoder()
{
	CCommonInfo::GetInstance()->KSNCOutputDebugString(L"NvidiaDecoder Destroyed - %s\n", m_wstrUrl.c_str());
	m_strUrl.clear();
	m_wstrUrl.clear();
};


MediaFrame::MediaFrame* NvidiaDecoder::Decode(AVPacket* avpkt)
{
	int ret = 0;

	// 초기화 여부 혹은 소멸 되어야하는지 여부 확인
	if (m_bCanDecode == false)
		return nullptr;

	if (avpkt->data == NULL)
		return nullptr;

	int nVideoBytes = 0, nFrameReturned = 0;

	if (m_NVdec == NULL)
	{
		int iGpu = CCommonInfo::GetInstance()->GetGpuNum();
		cuInit(0);
		int nGpu = 0;
		cuDeviceGetCount(&nGpu);
		if (iGpu < 0 || iGpu >= nGpu)
		{
			std::ostringstream err;
			err << "GPU ordinal out of range. Should be within [" << 0 << ", " << nGpu - 1 << "]" << std::endl;
			throw std::invalid_argument(err.str());
		}

		CUdevice cuDevice = 0;
		ck(cuDeviceGet(&cuDevice, 0));
		char szDeviceName[80];
		ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
		std::cout << "GPU in use: " << szDeviceName << std::endl;
		cuContext = NULL;
		ck(cuCtxCreate(&cuContext, CU_CTX_SCHED_AUTO, cuDevice));

		presenter0 = new FramePresenterD3D11(cuContext, 0, 0, 0, 0);

		// 현재 영상 스트림 FPS 가져옴		
		m_target_fps = av_q2d(m_pFmtCtx->streams[m_nVSI]->r_frame_rate);

		int decWidth = m_VideoWidth = m_pFmtCtx->streams[m_nVSI]->codecpar->width;
		int decHeight = m_VideoHeight = m_pFmtCtx->streams[m_nVSI]->codecpar->height;

		fpOut = new std::ofstream(szOutFilePath, std::ios::out | std::ios::binary);

		if (!fpOut)
		{
			std::ostringstream err;
			err << "Unable to open output file: " << szOutFilePath << std::endl;
			throw std::invalid_argument(err.str());
		}

		m_NVdec = new NvDecoder(cuContext, decWidth, decWidth, true, FFmpeg2NvCodecId(m_pFmtCtx->streams[m_nVSI]->codecpar->codec_id));

		std::cout << "CreateNvDecoder" << std::endl;
	}

	if (m_NVdec->Decode(avpkt->data, avpkt->size, &ppFrame, &nFrameReturned) == false)
	{
		CCommonInfo::GetInstance()->KSNCOutputDebugString(L"Decode Fail \n");
	}
	
	if (!nFrame && nFrameReturned)
	{
		std::string videoinfo = m_NVdec->GetVideoInfo();
		CCommonInfo::GetInstance()->KSNCOutputDebugString(L"VideoInfo = %s\n", StringTools::utf8_to_utf16(videoinfo).c_str());
	}

	nFrame += nFrameReturned;

	MediaFrame::MediaFrame *mFrame = nullptr;

	if (nFrameReturned == 0)
		return nullptr;

	for (int i = 0; i < nFrameReturned; i++)
	{
		if (i > 1)
			break;

		if (m_NVdec->GetBitDepth() == 8)
		{
			int framesize = m_NVdec->GetFrameSize();

			if (framesize == 0)
				return nullptr;

			//uint8_t *dpFrame = new uint8_t[framesize];
			//memcpy(dpFrame, (uint8_t *)ppFrame[i], framesize);
			//memcpy(dpFrame, reinterpret_cast<uint8_t*>(ppFrame[i]), m_NVdec->GetFrameSize());
			
			//CUdeviceptr			dpTempFrame = 0;
			//ck(cuMemAlloc(&dpTempFrame, 192 * 106 * 4));

			int decWidth, decHeight, decFramePitch;
			decWidth = m_NVdec->GetWidth();
			decHeight = m_NVdec->GetHeight();
			decFramePitch = m_NVdec->GetDeviceFramePitch();
			//ResizeNv12((uint8_t *)dpTempFrame, 192, 192, 106,
			//	(uint8_t *)ppFrame[i], m_NVdec->GetDeviceFramePitch(), m_NVdec->GetWidth(), m_NVdec->GetHeight());

			//fpOut->write(reinterpret_cast<char*>(ppFrame[i]), framesize);

			nFrame++;

			//*
			CUdeviceptr			dpTempFrame = 0;
			ck(cuMemAlloc(&dpTempFrame, m_NVdec->GetWidth() * m_NVdec->GetHeight() * 4));
			Nv12ToBgra32((uint8_t *)ppFrame[i], m_NVdec->GetWidth(), (uint8_t *)dpTempFrame, 4 * m_NVdec->GetWidth(), m_NVdec->GetWidth(), m_NVdec->GetHeight());

			int pitch = m_NVdec->GetWidth() * 4;

			//presenter0->PresentDeviceFrame((uint8_t *)dpTempFrame, pitch);
			
			//presenter0->LockFrameFresenter();
			presenter0->UpdateDeviceFramePos((uint8_t *)dpTempFrame, pitch, 0, 0, 0, 0, m_NVdec->GetWidth(), m_NVdec->GetHeight());
			presenter0->SwapChainPresent();
			//presenter0->UnLockFrameFresenter();
			
			//fpOut->write(reinterpret_cast<char*>(s), 4 * m_NVdec->GetWidth() * m_NVdec->GetHeight());
			/*
			int pitch = m_NVdec->GetWidth();// *4;

			int linesize[3] = { pitch , pitch, framesize };

			mFrame = MediaFrame::CreateMediaFrame(
				m_NVdec->GetWidth(),
				m_NVdec->GetHeight(),
				linesize, NVDecBGR);

			bool ret = MediaFrame::UpdateMeidaFrame(
				mFrame,
				m_NVdec->GetWidth(), m_NVdec->GetHeight(),
				(uint8_t *)dpTempFrame, nullptr, nullptr,
				linesize, nullptr, 0);

			if (!ret)
			{
				m_bGotIFrame = true;
			}
			//*/

			m_VideoWidth = m_NVdec->GetWidth();
			m_VideoHeight = m_NVdec->GetHeight();
			//mFrame->format = NVDecBGR;// frame->format;
			//delete &ppFrame;
			m_bGotIFrame = true;
			//;

			/*if (nFrame > 300)
			{
				fpOut->close();
				std::cout << "Total frame decoded: " << nFrame << std::endl << "Saved in file " << szOutFilePath;
			}*/
		}
		//m_NVdec->UnlockFrame(ppFrame, nFrameReturned);
		//else
			//P016ToBgra32((uint8_t *)ppFrame[i], 2 * m_NVdec->GetWidth(), (uint8_t *)dpFrame, 4 * m_NVdec->GetWidth(), m_NVdec->GetWidth(), m_NVdec->GetHeight());
	}

	// 현재 영상 스트림 FPS 업데이트 (변경시 바로 적용)		
	//m_target_fps = av_q2d(m_pFmtCtx->streams[m_nVSI]->avg_frame_rate);
	m_target_fps = av_q2d(m_pFmtCtx->streams[m_nVSI]->r_frame_rate);
				
	return mFrame;
}

MediaFrame::MediaFrame* NvidiaDecoder::Decode(const unsigned char* src_buf, int src_size)
{
	int ret = 0;

	// 초기화 여부 혹은 소멸 되어야하는지 여부 확인
	if (m_bCanDecode == false) return nullptr;

	//CUdeviceptr dpFrame = 0;
	//cuMemAlloc(&dpFrame, m_VideoHeight * m_VideoWidth * 4);

	int nVideoBytes = 0, nFrameReturned = 0;
	/*uint8_t **ppFrame;*/

	m_NVdec->Decode(src_buf, src_size, &ppFrame, &nFrameReturned);

	if (!nFrame && nFrameReturned)
	{
		std::string videoinfo = m_NVdec->GetVideoInfo();
		CCommonInfo::GetInstance()->KSNCOutputDebugString(L"VideoInfo = %s\n", StringTools::utf8_to_utf16(videoinfo).c_str());
	}

	nFrame += nFrameReturned;

	MediaFrame::MediaFrame *mFrame = nullptr;

	for (int i = 0; i < nFrameReturned; i++)
	{
		//if (m_NVdec->GetBitDepth() == 8)
		{
			int framesize = m_NVdec->GetFrameSize();

			if (framesize == 0)
				return nullptr;

			uint8_t *dpFrame = new uint8_t[framesize];
			//memset(dpFrame, 0x00, framesize);
			memcpy(dpFrame, (uint8_t *)ppFrame[i], framesize);
			//memcpy(dpFrame, reinterpret_cast<uint8_t*>(ppFrame[i]), m_NVdec->GetFrameSize());

			//Nv12ToBgra32((uint8_t *)ppFrame[i], m_NVdec->GetWidth(), (uint8_t *)dpFrame, 4 * m_NVdec->GetWidth(), m_NVdec->GetWidth(), m_NVdec->GetHeight());

			int pitch = m_NVdec->GetWidth();// *4;

			int linesize[2] = { pitch , pitch };

			mFrame = MediaFrame::CreateMediaFrame(
				m_NVdec->GetWidth(),
				m_NVdec->GetHeight(),
				linesize, NV12);

			MediaFrame::UpdateMeidaFrame(
				mFrame,
				m_NVdec->GetWidth(), m_NVdec->GetHeight(),
				dpFrame, dpFrame + (1920 * 1080), nullptr,
				linesize, nullptr, 0);

			m_VideoWidth = m_NVdec->GetWidth();
			m_VideoHeight = m_NVdec->GetHeight();
			mFrame->format = NV12;// frame->format;
			//delete &ppFrame;
			m_bGotIFrame = true;
			//;
		}
		//else
			//P016ToBgra32((uint8_t *)ppFrame[i], 2 * m_NVdec->GetWidth(), (uint8_t *)dpFrame, 4 * m_NVdec->GetWidth(), m_NVdec->GetWidth(), m_NVdec->GetHeight());
	}

	// 현재 영상 스트림 FPS 업데이트 (변경시 바로 적용)		
	//m_target_fps = av_q2d(m_pFmtCtx->streams[m_nVSI]->avg_frame_rate);
	m_target_fps = av_q2d(m_pFmtCtx->streams[m_nVSI]->r_frame_rate);

	return mFrame;
}

int NvidiaDecoder::InitDecoder(AVFormatContext* context)
{
	auto timecheck = Stopwatch::mark_now();
	int ret = -1;
	AVDictionary* opts = nullptr;
	AVCodec* pVideoCodec = NULL;
	av_init_packet(&m_avPacket);

	if (context == NULL)
	{
		//// Open media file.
		//ret = avformat_open_input(&m_pFmtCtx, m_strUrl.c_str(), NULL, NULL);
		//if (ret < 0)
		//{
		//	CCommonInfo::GetInstance()->KSNCOutputDebugString(
		//		L"NvidiaDecoder avformat_open_input error : %d - %s\n", ret, m_wstrUrl.c_str());
		//	CCommonInfo::GetInstance()->WriteLog(L"ERROR",
		//		L"NvidiaDecoder avformat_open_input error : %d - %s\n", ret, m_wstrUrl.c_str());
		//	return ret;
		//}
		return -1;
	}
	else
	{
		m_pFmtCtx = context;
		//m_pFmtCtx->probesize = 500000;
		//m_pFmtCtx->max_analyze_duration = 500000;
		//m_pFmtCtx->max_delay = 500000; // 0.5 sec
		//m_pFmtCtx->fps_probe_size = 30;
	}

	//for (unsigned int i = 0; i < m_pFmtCtx->nb_streams; i++)
	//{
	//	if (m_nVSI < 0 && m_pFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
	//	{
	//		m_nVSI = i;
	//		break;
	//	}
	//}

	m_nVSI = av_find_best_stream(m_pFmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &pVideoCodec, 0);

	if (m_nVSI < 0)
	{
		CCommonInfo::GetInstance()->KSNCOutputDebugString(
			L"FFMpegDecoder No Video streams were found - %s\n", m_wstrUrl.c_str());
		CCommonInfo::GetInstance()->WriteLog(L"ERROR",
			L"FFMpegDecoder No Video streams were found - %s\n", m_wstrUrl.c_str());

		av_packet_unref(&m_avPacket);
		return -1;
	}

	//int decWidth = m_VideoWidth = m_pFmtCtx->streams[m_nVSI]->codecpar->width;
	//int decHeight = m_VideoHeight = m_pFmtCtx->streams[m_nVSI]->codecpar->height;
	
	//CUdeviceptr			dpTempFrame = 0;
	//ck(cuMemAlloc(&dpFrame, decWidth * decHeight * 4));

	//int iGpu = 1;
	

	/*int iGpu = CCommonInfo::GetInstance()->GetGpuNum();
	cuInit(0);
	int nGpu = 0;
	cuDeviceGetCount(&nGpu);
	if (iGpu < 0 || iGpu >= nGpu)
	{
		std::ostringstream err;
		err << "GPU ordinal out of range. Should be within [" << 0 << ", " << nGpu - 1 << "]" << std::endl;
		throw std::invalid_argument(err.str());
	}

	CUdevice cuDevice = 1;
	ck(cuDeviceGet(&cuDevice, 1));
	char szDeviceName[80];
	ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
	std::cout << "GPU in use: " << szDeviceName << std::endl;
	CUcontext cuContext0 = NULL;
	ck(cuCtxCreate(&cuContext0, CU_CTX_SCHED_AUTO, cuDevice));

	Stopwatch decoding_stopwatch = Stopwatch::mark_now();

	presenter0 = new FramePresenterD3D11(cuContext0, 1920, 1080, 0, 0);*/

	m_bCanDecode = true;

	CCommonInfo::GetInstance()->KSNCOutputDebugString(
		L"NvidiaDecoder InitDecoder (success) time : %f msec - %s\n", timecheck.past_milliseconds(), m_wstrUrl.c_str());

	return 0;
}

int NvidiaDecoder::CleanupDecoder()
{
	m_bMustClose = true;
	m_bCanRead = false;

	if (m_bCanDecode)
		av_packet_unref(&m_avPacket);

	if (m_swsContext != NULL)
	{
		sws_freeContext(m_swsContext);
		m_swsContext = NULL;
	}

	delete m_NVdec;
	ck(cuCtxDestroy(cuContext));

	if (m_pFmtCtx)
	{
		/*for (unsigned int i = 0; i < m_pFmtCtx->nb_streams; i++)
		{
			avcodec_close(m_pFmtCtx->streams[i]->codec);
		}*/

		if (m_bCanDecode && m_pVCtx != NULL)
		{
			//avcodec_flush_buffers(m_pVCtx);
			try {
				avcodec_close(m_pVCtx);
			}
			catch (...) {
				CCommonInfo::GetInstance()->WriteLog(L"ERROR", L"Exeption when avcodec_close\n");
				CCommonInfo::GetInstance()->KSNCOutputDebugString(L"Exeption when avcodec_close\n");

				m_bCanDecode = false;
				m_nVSI = -1;
				m_pVCtx = NULL;
			}
		}
		//avcodec_close(m_pVCtx);
		//avformatContext close는 Client에서 하도록 위치 변경
		//avformat_close_input(&m_pFmtCtx);
	}

	m_bCanDecode = false;
	m_nVSI = -1;
	m_pVCtx = NULL;

	return 0;
}

bool		NvidiaDecoder::GetGotIFrame() { return m_bGotIFrame; }	// I-frame 수신 여부를 알려준다.
double		NvidiaDecoder::GetTargetFPS() { return m_target_fps; }
AVCodecID	NvidiaDecoder::GetCodecID() { return m_pFmtCtx->streams[m_nVSI]->codecpar->codec_id; }
int			NvidiaDecoder::GetVideoWidth() { if (m_pVCtx != NULL) return m_VideoWidth; return 0; }
int			NvidiaDecoder::GetVideoHeight() { if (m_pVCtx != NULL) return m_VideoHeight; return 0; }
int			NvidiaDecoder::GetVStreamInx() { if (this != NULL) return m_nVSI; return -1; }
int			NvidiaDecoder::GetLevel() { if (this != NULL) return m_DecodeLevel; return -1; }
void		NvidiaDecoder::SetLevel(int level) { if (this != NULL)m_DecodeLevel = level; }

int NvidiaDecoder::ClearDecoder()
{
	if (m_bCanDecode && m_pVCtx != NULL)
		avcodec_flush_buffers(m_pVCtx);
	return 0;
}

int NvidiaDecoder::inner_decode(AVFrame *frame, AVPacket *pkt)
{
	int ret;

	if (pkt) {
		ret = avcodec_send_packet(m_pVCtx, pkt);
		// In particular, we don't expect AVERROR(EAGAIN), because we read all
		// decoded frames with avcodec_receive_frame() until done.
		if (ret < 0)
			return ret == AVERROR_EOF ? 0 : ret;
	}

	ret = avcodec_receive_frame(m_pVCtx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return ret;

	return ret;
}

void NvidiaDecoder::ScaleLock()
{
	m_swsScaleMutex.lock();
}
void NvidiaDecoder::ScaleunLock()
{
	m_swsScaleMutex.unlock();
}

inline cudaVideoCodec NvidiaDecoder::FFmpeg2NvCodecId(AVCodecID id) {
	switch (id) {
	case AV_CODEC_ID_MPEG1VIDEO: return cudaVideoCodec_MPEG1;
	case AV_CODEC_ID_MPEG2VIDEO: return cudaVideoCodec_MPEG2;
	case AV_CODEC_ID_MPEG4: return cudaVideoCodec_MPEG4;
	case AV_CODEC_ID_VC1: return cudaVideoCodec_VC1;
	case AV_CODEC_ID_H264: return cudaVideoCodec_H264;
	case AV_CODEC_ID_HEVC: return cudaVideoCodec_HEVC;
	case AV_CODEC_ID_VP8: return cudaVideoCodec_VP8;
	case AV_CODEC_ID_VP9: return cudaVideoCodec_VP9;
	case AV_CODEC_ID_MJPEG: return cudaVideoCodec_JPEG;
	default: return cudaVideoCodec_NumCodecs;
	}
}
