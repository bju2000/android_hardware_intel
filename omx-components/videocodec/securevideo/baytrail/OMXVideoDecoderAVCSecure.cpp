/*
* Copyright (c) 2009-2014 Intel Corporation.  All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "OMXVideoDecoder"
#include <utils/Log.h>
#include "OMXVideoDecoderAVCSecure.h"
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <byteswap.h>

#define LOGVAR(v)   LOGD("LOGVAR: " #v " = %d", v)

// Be sure to have an equal string in VideoDecoderHost.cpp (libmix)
static const char* AVC_MIME_TYPE = "video/avc";
static const char* AVC_SECURE_MIME_TYPE = "video/avc-secure";

// Temp placeholder for NALU merge
static uint8_t config_nalu[1024];
static uint32_t config_nalu_len;
// Merge NALU for config (SPS & PPS) with Slice data for MDRM.
static void update_config_nalu(uint8_t* nalu_data, uint32_t* nalu_size)
{
    //move NALU for encrypted portion behind config info
    uint8_t temp[1024];
    memset(temp, 0, 1024);

    uint32_t* enc_dword_ptr = NULL;
    uint32_t enc_num_nalus = 0;
    uint32_t* clr_dword_ptr = NULL;
    uint32_t clr_num_nalus = 0;
    uint32_t len = 0;
    uint32_t t_len = 0;
    uint32_t offset = 0;

    if (*nalu_size > 4) {
        memcpy(temp, nalu_data+4, ((*nalu_size)-4));
    } else {
        LOGI("NALU size < 4! \n");
        return;
    }

    enc_dword_ptr = (uint32_t*)nalu_data;
    enc_num_nalus = bswap_32(*enc_dword_ptr);
    clr_dword_ptr = (uint32_t*)config_nalu;
    clr_num_nalus = bswap_32(*clr_dword_ptr);

    clr_dword_ptr = (uint32_t *)(config_nalu+8);
    len = bswap_32(*clr_dword_ptr);
    len = ((len+3)>>2)<<2;
    t_len = 16 + len;
    for( uint32_t i = 0; i < clr_num_nalus; i++ )
    {
        memcpy( nalu_data+offset, config_nalu+offset, t_len );
        enc_dword_ptr = (uint32_t *)(config_nalu+offset+t_len-len-12);
        (*enc_dword_ptr) = bswap_32(0);
        offset += t_len;
        clr_dword_ptr = (uint32_t *)(config_nalu+offset+4);
        len = bswap_32(*clr_dword_ptr);
        len = ((len+3)>>2)<<2;
        t_len = 12 + len;
    }

    // ignore first 4 len bytes
    memcpy((nalu_data + config_nalu_len), temp, ((*nalu_size)-4));

    enc_dword_ptr = (uint32_t*)nalu_data;
    (*enc_dword_ptr) = bswap_32(enc_num_nalus + clr_num_nalus);
    enc_num_nalus = (*enc_dword_ptr);
    *nalu_size = ((*nalu_size) + config_nalu_len - 4);

    enc_dword_ptr = (uint32_t*)nalu_data;
    enc_dword_ptr++;
    (*enc_dword_ptr) = bswap_32(0);

}

OMXVideoDecoderAVCSecure::OMXVideoDecoderAVCSecure()
    : mpLibInstance(NULL),
      mPAVPAppID(0xFF),
      mDropUntilIDR(false) {
    LOGV("OMXVideoDecoderAVCSecure is constructed.");
    LOGI("MDRM_OMX_VERSION: %.4f \n", MDRM_OMX_VERSION);
    mVideoDecoder = createVideoDecoder(AVC_SECURE_MIME_TYPE);
    if (!mVideoDecoder) {
        LOGE("createVideoDecoder failed for \"%s\"", AVC_SECURE_MIME_TYPE);
    }
    // Override default native buffer count defined in the base class
    mNativeBufferCount = OUTPORT_NATIVE_BUFFER_COUNT;

    memset(mOMXSecureBuffers, 0, sizeof(mOMXSecureBuffers));
    memset(mParsedFrames, 0, sizeof(mParsedFrames));

    BuildHandlerList();
}

OMXVideoDecoderAVCSecure::~OMXVideoDecoderAVCSecure() {
    // Cleanup any buffers that weren't freed
    for(int i = 0; i < INPORT_ACTUAL_BUFFER_COUNT; i++) {
        if(mOMXSecureBuffers[i]) {
            delete mOMXSecureBuffers[i];
        }
    }
    LOGV("OMXVideoDecoderAVCSecure is destructed.");
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::InitInputPortFormatSpecific(OMX_PARAM_PORTDEFINITIONTYPE *paramPortDefinitionInput) {
    // OMX_PARAM_PORTDEFINITIONTYPE
    paramPortDefinitionInput->nBufferCountActual = INPORT_ACTUAL_BUFFER_COUNT;
    paramPortDefinitionInput->nBufferCountMin = INPORT_MIN_BUFFER_COUNT;
    paramPortDefinitionInput->nBufferSize = INPORT_BUFFER_SIZE;
    paramPortDefinitionInput->format.video.cMIMEType = (OMX_STRING)AVC_MIME_TYPE;
    paramPortDefinitionInput->format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;

    // OMX_VIDEO_PARAM_AVCTYPE
    memset(&mParamAvc, 0, sizeof(mParamAvc));
    SetTypeHeader(&mParamAvc, sizeof(mParamAvc));
    mParamAvc.nPortIndex = INPORT_INDEX;
    // TODO: check eProfile/eLevel
    mParamAvc.eProfile = OMX_VIDEO_AVCProfileHigh; //OMX_VIDEO_AVCProfileBaseline;
    mParamAvc.eLevel = OMX_VIDEO_AVCLevel41; //OMX_VIDEO_AVCLevel1;

    // Set memory allocator
    this->ports[INPORT_INDEX]->SetMemAllocator(MemAllocSecure, MemFreeSecure, this);

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorStop(void) {
    // destroy PAVP session
    if(mpLibInstance) {
        pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
        LOGI("Destroying the PAVP session...\n");
        rc = mpLibInstance->pavp_destroy_session();
            if (rc != pavp_lib_session::status_ok)
                LOGE("pavp_destroy_session failed with error 0x%x\n", rc);
    }
    return OMXVideoDecoderBase::ProcessorStop();
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorProcess(
        OMX_BUFFERHEADERTYPE ***pBuffers,
        buffer_retain_t *retains,
        OMX_U32 numberBuffers) {

    OMX_ERRORTYPE ret;
    ret = OMXVideoDecoderBase::ProcessorProcess(pBuffers, retains, numberBuffers);
    if (ret != OMX_ErrorNone) {
        LOGE("OMXVideoDecoderBase::ProcessorProcess failed. Result: %#x", ret);
        return ret;
    }

    if (mDropUntilIDR) {
        retains[OUTPORT_INDEX] = BUFFER_RETAIN_NOT_RETAIN;
        OMX_BUFFERHEADERTYPE *pOutput = *pBuffers[OUTPORT_INDEX];
        pOutput->nFilledLen = 0;
        return OMX_ErrorNone;
    }

    return ret;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::PrepareConfigBuffer(VideoConfigBuffer *p) {
    OMX_ERRORTYPE ret;
    ret = OMXVideoDecoderBase::PrepareConfigBuffer(p);
    CHECK_RETURN_VALUE("OMXVideoDecoderBase::PrepareConfigBuffer");
    p->flag |=  WANT_SURFACE_PROTECTION;
    return ret;
}

// Create PAVP Session & retrieve associated PAVP AppID.
OMX_ERRORTYPE OMXVideoDecoderAVCSecure::CreatePavpSession(void) {
    if(!mpLibInstance) {
        LOGE("mpLibInstance is NULL!");
        return OMX_ErrorNotReady;
    }
    pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;

    LOGI("PAVP Heavy session creation...\n");

    rc = mpLibInstance->pavp_create_session(true);
    if (rc != pavp_lib_session::status_ok) {
        LOGE("PAVP Heavy: pavp_create_session failed with error 0x%x", rc);
        return OMX_ErrorNotReady;
    }

    LOGI("Get AppId of the PAVP Heavy session...\n");

    rc = mpLibInstance->pavp_get_app_id(reinterpret_cast<UINT&>(mPAVPAppID));
    if (rc != pavp_lib_session::status_ok) {
        LOGE("PAVP Heavy: pavp_get_app_id failed with error 0x%x", rc);
        return OMX_ErrorNotReady;
    } else {
        LOGI("pavp_get_app_id succesful, uiAppId = 0x%x\n", mPAVPAppID);
    }
    return OMX_ErrorNone;
}
//PAVP SecPassThrough to communicate with SEC.
OMX_ERRORTYPE OMXVideoDecoderAVCSecure::SecPassThrough(uint8_t* pInput, uint32_t  inSize, uint8_t* pOutput, uint32_t outSize) {

    pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;

    if(!mpLibInstance) {
        LOGE("mpLibInstance is NULL!");
        return OMX_ErrorNotReady;
    }

    rc = mpLibInstance->sec_pass_through(
                        reinterpret_cast<BYTE*>(pInput),
                        inSize,
                        reinterpret_cast<BYTE*>(pOutput),
                        outSize);

    if (rc != pavp_lib_session::status_ok) {
        LOGE("PAVP Failed: 0x%x", rc);
        return OMX_ErrorNotReady;
    }
    PAVP_CMD_HEADER *pHeader = (PAVP_CMD_HEADER*)pOutput;
    if (pHeader->Status) {
        LOGE("SEC Failed: SecPassThrough: FAILED 0x%x", pHeader->Status);
        return OMX_ErrorNotReady;
    }
    return OMX_ErrorNone;
}

// MDRM - Key injection
OMX_ERRORTYPE OMXVideoDecoderAVCSecure::MdrmInjectKey(uint8_t in_session_id, uint8_t* in_key_id) {
    LOGV("%s", __FUNCTION__);
    if(!mpLibInstance) {
        LOGE("mpLibInstance is NULL!");
        return OMX_ErrorNotReady;
    }
    wv2_inject_key_in input;
    wv2_inject_key_out output;
    transcript_conf conf;

    conf.drm_type_1_0 = 2;//PR 1 //SS
    conf.dest_encrypt_mode_25_24 = 1;//PR 0

    input.conf = conf;
    input.Header.ApiVersion = MDRM_API_VERSION;
    input.Header.CommandId =  wv2_inject_key;
    input.Header.Status = 0;
    input.Header.BufferLength = sizeof(input)-sizeof(PAVP_CMD_HEADER);
    input.session_id = in_session_id;
    input.StreamId = mPAVPAppID;
    memcpy(input.key_id, in_key_id, 16);

    return SecPassThrough((uint8_t*)&input, sizeof(input), (uint8_t*)&output, sizeof(output));
}

// Classic - Key injection
OMX_ERRORTYPE OMXVideoDecoderAVCSecure::WvSetTranscriptKey(void) {
    LOGV("%s", __FUNCTION__);
    if(!mpLibInstance) {
        LOGE("mpLibInstance is NULL!");
        return OMX_ErrorNotReady;
    }
    wv_set_xcript_key_in input;
    wv_set_xcript_key_out output;

    input.Header.ApiVersion = WV_API_VERSION;
    input.Header.CommandId =  wv_set_xcript_key;
    input.Header.Status = 0;
    input.StreamId = mPAVPAppID;
    input.Header.BufferLength = sizeof(input)-sizeof(PAVP_CMD_HEADER);

    return SecPassThrough((uint8_t*)&input, sizeof(input), (uint8_t*)&output, sizeof(output));
}

//Classic ProcessVideoFrame
OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ClassicProcessVideoFrame(SECVideoBuffer *secBuffer, uint32_t *parsed_data_size) {
    OMX_ERRORTYPE ret = OMX_ErrorNone;

    LOGV("%s", __FUNCTION__);

    if(!mpLibInstance) {
        LOGE("mpLibInstance is NULL!");
        return OMX_ErrorNotReady;
    }

    wv_heci_process_video_frame_in input;
    wv_heci_process_video_frame_out output;

    pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
    input.Header.ApiVersion = WV_API_VERSION;
    input.Header.CommandId = wv_process_video_frame;
    input.Header.Status = 0;
    input.Header.BufferLength = sizeof(input) - sizeof(PAVP_CMD_HEADER);
    input.num_of_packets = secBuffer->pes_packet_count;
    input.is_frame_not_encrypted = secBuffer->clear;
    input.src_offset = secBuffer->base_offset + secBuffer->partitions.src.offset;
    input.dest_offset = secBuffer->base_offset + secBuffer->partitions.dest.offset;
    input.metadata_offset = secBuffer->base_offset + secBuffer->partitions.metadata.offset;
    input.header_offset = secBuffer->base_offset + secBuffer->partitions.headers.offset;

    memset(&output, 0, sizeof(wv_heci_process_video_frame_out));

    ret = SecPassThrough((uint8_t*)&input, sizeof(input), (uint8_t*)&output, sizeof(output));
    *parsed_data_size = output.parsed_data_size;
    memcpy(secBuffer->iv, output.iv, 16);
    secBuffer->mdrm_info.config_len = 0;

    return ret;
}

//MDRM ProcessVideoFrame
OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ModularProcessVideoFrame(SECVideoBuffer *secBuffer, uint32_t *parsed_data_size) {
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    LOGV("%s", __FUNCTION__);
    if(!mpLibInstance) {
        LOGE("mpLibInstance is NULL!");
        return OMX_ErrorNotReady;
    }

    wv2_process_video_frame_in input;
    wv2_process_video_frame_out output;
    transcript_conf conf;
    memset(&conf, 0, sizeof(conf));
    memset(&input, 0, sizeof(input));
    memset(&output, 0, sizeof(wv2_process_video_frame_out));

    input.Header.ApiVersion = MDRM_API_VERSION;
    input.Header.CommandId = wv2_process_video_frame;
    input.Header.Status = 0;
    input.Header.BufferLength = sizeof(input) - sizeof(PAVP_CMD_HEADER);

    memcpy(input.key_id, secBuffer->mdrm_info.key_id, 16);
    input.key_id_len = secBuffer->mdrm_info.key_id_len;
    input.session_id = secBuffer->mdrm_info.session_id; //mSessionID?
    input.header_offset = secBuffer->base_offset + secBuffer->partitions.headers.offset;
    input.dest_offset = secBuffer->base_offset + secBuffer->partitions.dest.offset;

    conf.drm_type_1_0 = 2;//PR 1 //SS
    conf.dest_encrypt_mode_25_24 = 1;//PR 0
    if ( secBuffer->mdrm_info.config_len) {
	memset(config_nalu, 0, sizeof(config_nalu));
	pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
	conf.num_headers_or_packets_15_8 =  2;  //SPS/PPS
	conf.src_encrypt_mode_17_16 = 0;//clear=0;//PR 1
	input.conf = conf;
	input.frame_offset = secBuffer->base_offset + secBuffer->partitions.src.offset;
	input.metadata_offset = secBuffer->base_offset + secBuffer->partitions.metadata.offset;

        ret = SecPassThrough((uint8_t*)&input, sizeof(input), (uint8_t*)&output, sizeof(output));

	if(ret == OMX_ErrorNone) {
	    // Assemble parsed frame information
	    // NALU data
	    uint8_t *nalu_data = secBuffer->base + secBuffer->partitions.headers.offset;
	    uint32_t nalu_data_size = output.parsed_data_size;
	    memcpy(config_nalu, nalu_data, nalu_data_size);
	    config_nalu_len = nalu_data_size;
	}
    }

    if ( ret == OMX_ErrorNone) {
	conf.num_headers_or_packets_15_8 =  secBuffer->pes_packet_count;  //SS
	conf.src_encrypt_mode_17_16 = secBuffer->clear?0:1;//clear=0;//PR 1
	input.conf = conf;

	if (secBuffer->mdrm_info.config_len) {
	    input.metadata_offset =SEC_DMA_ALIGN( secBuffer->base_offset + secBuffer->partitions.metadata.offset + 24);
	    input.frame_offset = secBuffer->mdrm_info.config_frame_offset;
	} else {
	    input.frame_offset = secBuffer->base_offset + secBuffer->partitions.src.offset;
	    input.metadata_offset = secBuffer->base_offset + secBuffer->partitions.metadata.offset;
	}
        ret = SecPassThrough((uint8_t*)&input, sizeof(input), (uint8_t*)&output, sizeof(output));
        *parsed_data_size = output.parsed_data_size;
        memcpy(secBuffer->iv, output.iv, 16);
   }
   return ret;
}

// Manage PAVP Session - destroy & re-create for Auto-tear-down
OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ManagePAVPSession(bool force_recreate) {
    LOGV("%s", __FUNCTION__);

    // PAVP auto teardown: check if PAVP session is alive
    bool balive = false;
    pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
    rc = mpLibInstance->pavp_is_session_alive(&balive);
    if (rc != pavp_lib_session::status_ok) {
        LOGE("pavp_is_session_alive failed with error 0x%x", rc);
        return  OMX_ErrorNotReady;
    }

    if (!balive || force_recreate) {

        LOGI("PAVP session is %s", balive?"active":"in-active\n");
        //Destroy & re-create
        LOGI("Destroying the PAVP session...\n");
        rc = mpLibInstance->pavp_destroy_session();
        if (rc != pavp_lib_session::status_ok) {
            LOGE("pavp_destroy_session failed with error 0x%x", rc);
            return  OMX_ErrorNotReady;
        }

        // Frames in the video decoder DPB are encrypted with the
        // PAVP heavy mode key (IED key) for the destroyed session.
        // Flush video decoder to remove them.
        mVideoDecoder->flush();

        mpLibInstance = NULL;
        mDropUntilIDR = true;
    }
    return OMX_ErrorNone;

}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::PrepareDecodeBuffer(OMX_BUFFERHEADERTYPE *buffer, buffer_retain_t *retain, VideoDecodeBuffer *p) {
    OMX_ERRORTYPE ret;
    ret = OMXVideoDecoderBase::PrepareDecodeBuffer(buffer, retain, p);
    CHECK_RETURN_VALUE("OMXVideoDecoderBase::PrepareDecodeBuffer");

    if (buffer->nFilledLen == 0) {
        return OMX_ErrorNone;
    }
    p->flag |= HAS_COMPLETE_FRAME;

    if (buffer->nOffset != 0) {
        LOGW("buffer offset %lu is not zero!!!", buffer->nOffset);
    }

    OMXSecureBuffer *secureBuffer = (OMXSecureBuffer*)buffer->pBuffer;
    if(!secureBuffer) {
        LOGE("OMXSecureBuffer is NULL");
        return OMX_ErrorBadParameter;
    }
    SECVideoBuffer *secBuffer = (SECVideoBuffer*)secureBuffer->secBuffer;
    if(!secureBuffer) {
        LOGE("SECVideoBuffer is NULL");
        return OMX_ErrorBadParameter;
    }

    // OMX_BUFFERFLAG_CODECCONFIG is an optional flag
    // if flag is set, buffer will only contain codec data.
    if ( (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG) &&
             (secBuffer->drm_type == DRM_TYPE_CLASSIC_WV)) {
        LOGV("Received AVC codec data.");
        return ret;
    }

    uint32_t parsed_data_size = 0;

    if(secBuffer->drm_type == DRM_TYPE_CLASSIC_WV) {

	if(!mpLibInstance && secBuffer->pLibInstance) {
            mpLibInstance = secBuffer->pLibInstance;
            if( CreatePavpSession() == OMX_ErrorNone) {
                ret = WvSetTranscriptKey();
            }
	}
        ret = ManagePAVPSession(ret == OMX_ErrorNotReady);

	if ( ret == OMX_ErrorNone) {
            ret = ClassicProcessVideoFrame(secBuffer, &parsed_data_size);
        }
    }
    else if(secBuffer->drm_type == DRM_TYPE_MDRM) {

	if(!mpLibInstance && secBuffer->pLibInstance) {
            mpLibInstance = secBuffer->pLibInstance;
            if( CreatePavpSession() == OMX_ErrorNone) {
                ret = MdrmInjectKey(secBuffer->mdrm_info.session_id, secBuffer->mdrm_info.key_id);
            }
        }
        ret = ManagePAVPSession(ret == OMX_ErrorNotReady);
	if( ret == OMX_ErrorNone) {
            ret = ModularProcessVideoFrame(secBuffer, &parsed_data_size);
        }
    } else  {
        LOGE("Invalid DRM_TYPE: 0x%x passed!", secBuffer->drm_type);
        ret = OMX_ErrorNotReady;
    }

    SECParsedFrame* parsedFrame = &(mParsedFrames[secureBuffer->index]);
    if(ret == OMX_ErrorNone) {
	// Assemble parsed frame information
	// NALU data
	parsedFrame->nalu_data = secBuffer->base + secBuffer->partitions.headers.offset;
	parsedFrame->nalu_data_size = parsed_data_size;
	if (secBuffer->mdrm_info.config_len) {
	    update_config_nalu(parsedFrame->nalu_data, &(parsedFrame->nalu_data_size));
	}
	memcpy(parsedFrame->pavp_info.iv, secBuffer->iv, 16);
        parsedFrame->pavp_info.app_id = mPAVPAppID;

	ret = ConstructFrameInfo(secBuffer->base + secBuffer->partitions.dest.offset, buffer->nFilledLen,
	    &(parsedFrame->pavp_info), parsedFrame->nalu_data, parsedFrame->nalu_data_size,
	    &(parsedFrame->frame_info));

	if((ret != OMX_ErrorNone) || (parsedFrame->frame_info.num_nalus == 0 )) {
	    LOGE("NALU parsing failed - num_nalus = 0!");
	    ret = OMX_ErrorNotReady;
	}
	if(mDropUntilIDR) {
	    bool idr = false;
	    for(uint32_t n = 0; n < parsedFrame->frame_info.num_nalus; n++) {
		if((parsedFrame->frame_info.nalus[n].type & 0x1F) == h264_NAL_UNIT_TYPE_IDR) {
		    idr = true;
		    break;
		}
	    }
	    if(idr) {
		LOGD("IDR frame found; restoring playback.");
		mDropUntilIDR = false;
	    } else {
		LOGD("Dropping non-IDR frame.");
		ret = OMX_ErrorNotReady;
	    }
	}
    }
    if(ret == OMX_ErrorNone) {
	// Pass frame info to VideoDecoderAVCSecure in VideoDecodeBuffer
	p->data = (uint8_t *)&(parsedFrame->frame_info);
	p->size = sizeof(frame_info_t);
	p->flag |= IS_SECURE_DATA;
    }

    return ret;
}

OMX_COLOR_FORMATTYPE OMXVideoDecoderAVCSecure::GetOutputColorFormat(int width, int height) {
    // BYT HWC expects Tiled output color format for all resolution
    return (OMX_COLOR_FORMATTYPE)OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar_Tiled;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::BuildHandlerList(void) {
    OMXVideoDecoderBase::BuildHandlerList();
    AddHandler(OMX_IndexParamVideoAvc, GetParamVideoAvc, SetParamVideoAvc);
    AddHandler(OMX_IndexParamVideoProfileLevelQuerySupported, GetParamVideoAVCProfileLevel, SetParamVideoAVCProfileLevel);
    AddHandler(static_cast<OMX_INDEXTYPE> (OMX_IndexExtEnableNativeBuffer), GetNativeBufferMode, SetNativeBufferMode);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::GetParamVideoAvc(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_AVCTYPE *p = (OMX_VIDEO_PARAM_AVCTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);

    memcpy(p, &mParamAvc, sizeof(*p));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::SetParamVideoAvc(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_AVCTYPE *p = (OMX_VIDEO_PARAM_AVCTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);
    CHECK_SET_PARAM_STATE();

    // TODO: do we need to check if port is enabled?
    // TODO: see SetPortAvcParam implementation - Can we make simple copy????
    memcpy(&mParamAvc, p, sizeof(mParamAvc));
    return OMX_ErrorNone;
}


OMX_ERRORTYPE OMXVideoDecoderAVCSecure::GetParamVideoAVCProfileLevel(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_PROFILELEVELTYPE *p = (OMX_VIDEO_PARAM_PROFILELEVELTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);
    CHECK_ENUMERATION_RANGE(p->nProfileIndex,1);

    p->eProfile = mParamAvc.eProfile;
    p->eLevel = mParamAvc.eLevel;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::SetParamVideoAVCProfileLevel(OMX_PTR pStructure) {
    LOGW("SetParamVideoAVCProfileLevel is not supported.");
    return OMX_ErrorUnsupportedSetting;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::GetNativeBufferMode(OMX_PTR pStructure) {
    LOGE("GetNativeBufferMode is not implemented");
    return OMX_ErrorNotImplemented;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::SetNativeBufferMode(OMX_PTR pStructure) {
    OMXVideoDecoderBase::SetNativeBufferMode(pStructure);
    PortVideo *port = NULL;
    port = static_cast<PortVideo *>(this->ports[OUTPORT_INDEX]);

    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    memcpy(&port_def,port->GetPortDefinition(),sizeof(port_def));
    port_def.format.video.eColorFormat = (OMX_COLOR_FORMATTYPE)OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar_Tiled;
    port->SetPortDefinition(&port_def,true);

    return OMX_ErrorNone;
}

OMX_U8* OMXVideoDecoderAVCSecure::MemAllocSecure(OMX_U32 nSizeBytes, OMX_PTR pUserData) {
    OMXVideoDecoderAVCSecure* p = (OMXVideoDecoderAVCSecure *)pUserData;
    if (p) {
        return p->MemAllocSecure(nSizeBytes);
    }
    LOGE("NULL pUserData.");
    return NULL;
}

void OMXVideoDecoderAVCSecure::MemFreeSecure(OMX_U8 *pBuffer, OMX_PTR pUserData) {
    OMXVideoDecoderAVCSecure* p = (OMXVideoDecoderAVCSecure *)pUserData;
    if (p) {
        p->MemFreeSecure(pBuffer);
        return;
    }
    LOGE("NULL pUserData.");
}

OMX_U8* OMXVideoDecoderAVCSecure::MemAllocSecure(OMX_U32 nSizeBytes) {
    // Ignore passed nSizeBytes, use INPORT_BUFFER_SIZE instead

    uint32_t index = 0;
    do {
        if(mOMXSecureBuffers[index] == NULL) {
            break;
        }
    } while(++index < INPORT_ACTUAL_BUFFER_COUNT);

    if(index >= INPORT_ACTUAL_BUFFER_COUNT) {
        LOGE("No free buffers");
        return NULL;
    }

    mOMXSecureBuffers[index] = new OMXSecureBuffer;
    if(!mOMXSecureBuffers[index]) {
        LOGE("Failed to allocate OMXSecureBuffer.");
        return NULL;
    }

    mOMXSecureBuffers[index]->index = index;
    // SEC buffer will by assigned by WVCrypto
    mOMXSecureBuffers[index]->secBuffer = NULL;

    return (OMX_U8*)mOMXSecureBuffers[index];
}

void OMXVideoDecoderAVCSecure::MemFreeSecure(OMX_U8 *pBuffer) {
    OMXSecureBuffer *p = (OMXSecureBuffer*) pBuffer;
    if (p == NULL) {
        return;
    }

    uint32_t index = p->index;
    if(mOMXSecureBuffers[index] == p) {
        delete(p);
        mOMXSecureBuffers[index] = NULL;
    } else {
        LOGE("ERROR: pBuffer (%p) does not match mOMXSecureBuffer[%d] pointer (%p)", p, index, mOMXSecureBuffers[index]);
    }
}

// Byteswap slice header (SEC returns the slice header with fields in big-endian byte order)
inline void byteswap_slice_header(slice_header_t* slice_header) {
    // Byteswap the fields of slice_header
    slice_header->first_mb_in_slice = bswap_32(slice_header->first_mb_in_slice);
    slice_header->frame_num = bswap_32(slice_header->frame_num);
    slice_header->idr_pic_id = bswap_16(slice_header->idr_pic_id);
    slice_header->pic_order_cnt_lsb = bswap_16(slice_header->pic_order_cnt_lsb);
    slice_header->delta_pic_order_cnt_bottom = bswap_32(slice_header->delta_pic_order_cnt_bottom);
    slice_header->delta_pic_order_cnt[0] = bswap_32(slice_header->delta_pic_order_cnt[0]);
    slice_header->delta_pic_order_cnt[1] = bswap_32(slice_header->delta_pic_order_cnt[1]);
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ConstructFrameInfo(
    uint8_t* frame_data,
    uint32_t frame_size,
    pavp_info_t* pavp_info,
    uint8_t* nalu_data,
    uint32_t nalu_data_size,
    frame_info_t* frame_info) {

    if(!frame_data || !pavp_info || !nalu_data) {
        LOGE("%s NULL input", __FUNCTION__);
        return OMX_ErrorNotReady;
    }

    uint32_t* dword_ptr = (uint32_t*)nalu_data;
    uint8_t* byte_ptr = NULL;
    uint32_t data_size = 0;

    frame_info->data = frame_data;
    frame_info->length = frame_size;
    frame_info->pavp = pavp_info;
    frame_info->dec_ref_pic_marking = NULL;

    // Byteswap nalu data (SEC returns fields in big-endian byte order)
    frame_info->num_nalus = bswap_32(*dword_ptr);
    dword_ptr++;
    for(uint32_t n = 0; n < frame_info->num_nalus; n++) {
        // Byteswap offset
        frame_info->nalus[n].offset = bswap_32(*dword_ptr);
        dword_ptr++;

       // Byteswap nalu_size
        frame_info->nalus[n].length = bswap_32(*dword_ptr);
        dword_ptr++;

        // Byteswap data_size
        data_size = bswap_32(*dword_ptr);
        dword_ptr++;

        byte_ptr = (uint8_t*)dword_ptr;
        frame_info->nalus[n].type = *byte_ptr;
        switch(frame_info->nalus[n].type & 0x1F) {
        case h264_NAL_UNIT_TYPE_SPS:
        case h264_NAL_UNIT_TYPE_PPS:
        case h264_NAL_UNIT_TYPE_SEI:
            // Point to cleartext in nalu data buffer
            frame_info->nalus[n].data = byte_ptr;
            frame_info->nalus[n].slice_header = NULL;
            break;
        case h264_NAL_UNIT_TYPE_SLICE:
        case h264_NAL_UNIT_TYPE_IDR:
            // Point to ciphertext in frame buffer
            frame_info->nalus[n].data = frame_info->data + frame_info->nalus[n].offset;
            byteswap_slice_header((slice_header_t*)byte_ptr);
            frame_info->nalus[n].slice_header = (slice_header_t*)byte_ptr;
            if(data_size > sizeof(slice_header_t)) {
                byte_ptr += sizeof(slice_header_t);
                frame_info->dec_ref_pic_marking = (dec_ref_pic_marking_t*)byte_ptr;
            }
            break;
        default:
            LOGE("ERROR: SEC returned an unsupported NALU type: %x", frame_info->nalus[n].type);
            frame_info->nalus[n].data = NULL;
            frame_info->nalus[n].slice_header = NULL;
            break;
        }

        // Advance to next NALU (including padding)
        dword_ptr += (data_size + 3) >> 2;
    }

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::SetMaxOutputBufferCount(OMX_PARAM_PORTDEFINITIONTYPE *p) {
    OMX_ERRORTYPE ret;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);

    p->nBufferCountActual = OUTPORT_NATIVE_BUFFER_COUNT;
    return OMXVideoDecoderBase::SetMaxOutputBufferCount(p);
}
DECLARE_OMX_COMPONENT("OMX.Intel.hw_vd.h264.secure", "video_decoder.avc", OMXVideoDecoderAVCSecure);
