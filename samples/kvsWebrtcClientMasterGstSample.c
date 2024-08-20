#include "Samples.h"
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>


extern PSampleConfiguration gSampleConfiguration;
// #define VERBOSE

GstElement* pipeline = NULL;


static UINT64 presentationTsIncrement = 0;
static BOOL eos = FALSE;


// Callback for audio frame received from the transceiver
VOID onGstAudioFrameReady(UINT64 customData, PFrame pFrame)
{
   STATUS retStatus = STATUS_SUCCESS;
   GstFlowReturn ret;
   GstBuffer* buffer;
   GstElement* appsrcAudio = (GstElement*) customData;


   CHK_ERR(appsrcAudio != NULL, STATUS_NULL_ARG, "appsrcAudio is null");
   CHK_ERR(pFrame != NULL, STATUS_NULL_ARG, "Audio frame is null");


   if (!eos) {
       buffer = gst_buffer_new_allocate(NULL, pFrame->size, NULL);
       CHK_ERR(buffer != NULL, STATUS_NULL_ARG, "Buffer allocation failed");


       DLOGV("Audio frame size: %d, presentationTs: %llu", pFrame->size, presentationTsIncrement);


       GST_BUFFER_DTS(buffer) = presentationTsIncrement;
       GST_BUFFER_PTS(buffer) = presentationTsIncrement;
       GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(pFrame->size, GST_SECOND, DEFAULT_AUDIO_OPUS_BYTE_RATE);


       if (gst_buffer_fill(buffer, 0, pFrame->frameData, pFrame->size) != pFrame->size) {
           DLOGE("Buffer fill did not complete correctly");
           gst_buffer_unref(buffer);
           return;
       }
       g_signal_emit_by_name(appsrcAudio, "push-buffer", buffer, &ret);
       if (ret != GST_FLOW_OK) {
           DLOGE("Error pushing buffer: %s", gst_flow_get_name(ret));
       }
       gst_buffer_unref(buffer);
   }


CleanUp:
   return;
}


// Callback for session shutdown event
VOID onSampleStreamingSessionShutdown(UINT64 customData, PSampleStreamingSession pSampleStreamingSession)
{
   (void) (pSampleStreamingSession);
   eos = TRUE;
   GstElement* pipeline = (GstElement*) customData;
   gst_element_send_event(pipeline, gst_event_new_eos());
}


// Callback for new samples received in the appsink
GstFlowReturn on_new_sample(GstElement* sink, gpointer data, UINT64 trackid)
{
   GstBuffer* buffer;
   STATUS retStatus = STATUS_SUCCESS;
   BOOL isDroppable, delta;
   GstFlowReturn ret = GST_FLOW_OK;
   GstSample* sample = NULL;
   GstMapInfo info;
   GstSegment* segment;
   GstClockTime buf_pts;
   Frame frame;
   STATUS status;
   PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) data;
   PSampleStreamingSession pSampleStreamingSession = NULL;
   PRtcRtpTransceiver pRtcRtpTransceiver = NULL;
   UINT32 i;
   guint bitrate;


   CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "NULL sample configuration");


   info.data = NULL;
   sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));


   buffer = gst_sample_get_buffer(sample);
   isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
       (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
       (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
       // drop if buffer contains header only and has invalid timestamp
       !GST_BUFFER_PTS_IS_VALID(buffer);


   if (!isDroppable) {
       delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);


       frame.flags = delta ? FRAME_FLAG_NONE : FRAME_FLAG_KEY_FRAME;


       // convert from segment timestamp to running time in live mode.
       segment = gst_sample_get_segment(sample);
       buf_pts = gst_segment_to_running_time(segment, GST_FORMAT_TIME, buffer->pts);
       if (!GST_CLOCK_TIME_IS_VALID(buf_pts)) {
           DLOGE("[KVS GStreamer Master] Frame contains invalid PTS dropping the frame");
       }


       if (!(gst_buffer_map(buffer, &info, GST_MAP_READ))) {
           DLOGE("[KVS GStreamer Master] on_new_sample(): Gst buffer mapping failed");
           goto CleanUp;
       }


       frame.trackId = trackid;
       frame.duration = 0;
       frame.version = FRAME_CURRENT_VERSION;
       frame.size = (UINT32) info.size;
       frame.frameData = (PBYTE) info.data;


       MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
       for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
           pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
           frame.index = (UINT32) ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);


           if (trackid == DEFAULT_AUDIO_TRACK_ID) {
               if (pSampleStreamingSession->pSampleConfiguration->enableTwcc && pipeline != NULL) {
                   GstElement* encoder = gst_bin_get_by_name(GST_BIN(pipeline), "sampleAudioEncoder");
                   if (encoder != NULL) {
                       g_object_get(G_OBJECT(encoder), "bitrate", &bitrate, NULL);
                       MUTEX_LOCK(pSampleStreamingSession->twccMetadata.updateLock);
                       pSampleStreamingSession->twccMetadata.currentAudioBitrate = (UINT64) bitrate;
                       if (pSampleStreamingSession->twccMetadata.newAudioBitrate != 0) {
                           bitrate = (guint) (pSampleStreamingSession->twccMetadata.newAudioBitrate);
                           pSampleStreamingSession->twccMetadata.newAudioBitrate = 0;
                           g_object_set(G_OBJECT(encoder), "bitrate", bitrate, NULL);
                       }
                       MUTEX_UNLOCK(pSampleStreamingSession->twccMetadata.updateLock);
                   }
               }
               pRtcRtpTransceiver = pSampleStreamingSession->pAudioRtcRtpTransceiver;
               frame.presentationTs = pSampleStreamingSession->audioTimestamp;
               frame.decodingTs = frame.presentationTs;
               pSampleStreamingSession->audioTimestamp += SAMPLE_AUDIO_FRAME_DURATION; // assume audio frame size is 20ms, which is default in opusenc
           }
           status = writeFrame(pRtcRtpTransceiver, &frame);
           if (status != STATUS_SRTP_NOT_READY_YET && status != STATUS_SUCCESS) {
#ifdef VERBOSE
               DLOGE("[KVS GStreamer Master] writeFrame() failed with 0x%08x", status);
#endif
           } else if (status == STATUS_SUCCESS && pSampleStreamingSession->firstFrame) {
               PROFILE_WITH_START_TIME(pSampleStreamingSession->offerReceiveTime, "Time to first frame");
               pSampleStreamingSession->firstFrame = FALSE;
           } else if (status == STATUS_SRTP_NOT_READY_YET) {
               DLOGI("[KVS GStreamer Master] SRTP not ready yet, dropping frame");
           }
       }
       MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
   }


CleanUp:


   if (info.data != NULL) {
       gst_buffer_unmap(buffer, &info);
   }


   if (sample != NULL) {
       gst_sample_unref(sample);
   }


   if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
       ret = GST_FLOW_EOS;
   }


   return ret;
}


GstFlowReturn on_new_sample_audio(GstElement* sink, gpointer data)
{
   if (!sink) {
       g_printerr("Error: Appsink is NULL\n");
       return GST_FLOW_ERROR;
   }
   return on_new_sample(sink, data, DEFAULT_AUDIO_TRACK_ID);
}


// Function to create and run the GStreamer pipeline for bidirectional audio streaming
PVOID receiveSendGstreamerAudio(PVOID args) {
   printf("receiveTest 1\n");
   STATUS retStatus = STATUS_SUCCESS;
   GstElement *appsrcAudio = NULL, *appsinkAudio = NULL, *pipeline = NULL;
   GstBus* bus;
   GstMessage* msg;
   GError* error = NULL;
   GstCaps *audiocaps;
   PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
   PSampleConfiguration pSampleConfiguration = pSampleStreamingSession->pSampleConfiguration;
   PCHAR roleType = "Master";


   gchar *audioDescription = "appsrc name=appsrc-audio ! queue ! opusparse ! decodebin ! autoaudiosink autoaudiosrc ! queue ! audioconvert ! audioresample ! opusenc name=sampleAudioEncoder ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio";


   // Initialize GStreamer
   if (!gst_init_check(NULL, NULL, &error)) {
       fprintf(stderr, "[KVS GStreamer %s] GStreamer initialization failed: %s\n", roleType, error->message);
       g_error_free(error);
       return (PVOID)(ULONG_PTR)STATUS_INTERNAL_ERROR;
   }
  
   if (!pSampleStreamingSession) {
       fprintf(stderr, "[KVS GStreamer %s] Sample streaming session is NULL\n", roleType);
       return (PVOID)(ULONG_PTR)STATUS_NULL_ARG;
   }


   // Create caps
   audiocaps = gst_caps_new_simple("audio/x-opus",
                                    "rate", G_TYPE_INT, DEFAULT_AUDIO_OPUS_SAMPLE_RATE_HZ,
                                    "channel-mapping-family", G_TYPE_INT, 1, NULL);
  
   // Launch pipeline
   pipeline = gst_parse_launch(audioDescription, &error);
   if (!pipeline) {
       fprintf(stderr, "[KVS GStreamer %s] Pipeline is NULL: %s\n", roleType, error->message);
       g_error_free(error);
       return (PVOID)(ULONG_PTR)STATUS_INTERNAL_ERROR;
   }


   // Get appsrc
   appsrcAudio = gst_bin_get_by_name(GST_BIN(pipeline), "appsrc-audio");
   if (!appsrcAudio) {
       fprintf(stderr, "[KVS GStreamer %s] Cannot find appsrc audio\n", roleType);
       gst_object_unref(pipeline);
       return (PVOID)(ULONG_PTR)STATUS_INTERNAL_ERROR;
   }


   // Set caps on appsrc
   g_object_set(G_OBJECT(appsrcAudio), "caps", audiocaps, NULL);
   gst_caps_unref(audiocaps);


   // Setup audio frame callback
   CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver, (UINT64)appsrcAudio, onGstAudioFrameReady));


   printf("receiveTest 2\n");


   // Get appsink
   appsinkAudio = gst_bin_get_by_name(GST_BIN(pipeline), "appsink-audio");
   if (!appsinkAudio) {
       fprintf(stderr, "[KVS GStreamer %s] Cannot find appsink audio\n", roleType);
       gst_object_unref(appsrcAudio);
       gst_object_unref(pipeline);
       return (PVOID)(ULONG_PTR)STATUS_INTERNAL_ERROR;
   }


   // Connect new-sample signal
   g_signal_connect(appsinkAudio, "new-sample", G_CALLBACK(on_new_sample_audio), (gpointer)pSampleConfiguration);


   printf("receiveTest 3\n");


   // Shutdown handling
   CHK_STATUS(streamingSessionOnShutdown(pSampleStreamingSession, (UINT64)pipeline, onSampleStreamingSessionShutdown));


   // Set pipeline to playing
   if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
       fprintf(stderr, "[KVS GStreamer %s] Failed to set pipeline to PLAYING state\n", roleType);
       goto CleanUp; // Use CleanUp label here
   }


   printf("receiveTest 4\n");


   // Block until error or EOS
   bus = gst_element_get_bus(pipeline);
   if (!bus) {
       fprintf(stderr, "[KVS GStreamer %s] Bus is NULL\n", roleType);
       goto CleanUp; // Use Clean label here
   }


   msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
   printf("receiveTest 5\n");


   // Handle message
   if (msg != NULL) {
       switch (GST_MESSAGE_TYPE(msg)) {
           case GST_MESSAGE_ERROR:
               gst_message_parse_error(msg, &error, NULL);
               fprintf(stderr, "Error received: %s\n", error->message);
               g_error_free(error);
               break;
           case GST_MESSAGE_EOS:
               printf("End of stream\n");
               break;
           default:
               break;
       }
       gst_message_unref(msg);
   }


CleanUp: // Declare Clean label here
   // Cleanup
   if (bus != NULL) {
       gst_object_unref(bus);
   }
   if (pipeline != NULL) {
       gst_element_set_state(pipeline, GST_STATE_NULL);
       printf("pipeline NULL\n");
       gst_object_unref(pipeline);
   } else {
       gst_element_set_state(pipeline, GST_STATE_NULL);
       printf("pipeline not but now NULL\n");
       gst_object_unref(pipeline);
   }
   if (appsrcAudio != NULL) {
       gst_object_unref(appsrcAudio);
   }
   if (appsinkAudio != NULL) {
       gst_object_unref(appsinkAudio);
   }


   // Final error cleanup
   if (error) {
       fprintf(stderr, "[KVS GStreamer %s] %s\n", roleType, error->message);
       g_clear_error(&error);
   }


   //printf("deinitializing\n");
   //gst_deinit();
   //printf("deinitialized\n");


   ATOMIC_STORE_BOOL(&pSampleConfiguration->interrupted, TRUE);
   return (PVOID)(ULONG_PTR)retStatus;
}






INT32 main(INT32 argc, CHAR* argv[])
{
   STATUS retStatus = STATUS_SUCCESS;
   PSampleConfiguration pSampleConfiguration = NULL;
   PCHAR pChannelName;
   RTC_CODEC audioCodec = RTC_CODEC_OPUS;
   RTC_CODEC videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;


   SET_INSTRUMENTED_ALLOCATORS();
   UINT32 logLevel = setLogLevel();


   signal(SIGINT, sigintHandler);


#ifdef IOT_CORE_ENABLE_CREDENTIALS
   CHK_ERR((pChannelName = argc > 1 ? argv[1] : GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION,
           "AWS_IOT_CORE_THING_NAME must be set");
#else
   pChannelName = argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME;
#endif


   CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_MASTER, TRUE, TRUE, logLevel, &pSampleConfiguration));


   pSampleConfiguration->audioSource = receiveSendGstreamerAudio;
   pSampleConfiguration->receiveAudioVideoSource = receiveSendGstreamerAudio;
   pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
   pSampleConfiguration->audioCodec = audioCodec;
   pSampleConfiguration->videoCodec = videoCodec;
  


#ifdef ENABLE_DATA_CHANNEL
   pSampleConfiguration->onDataChannel = onDataChannel;
#endif
   pSampleConfiguration->customData = (UINT64) pSampleConfiguration;
   pSampleConfiguration->srcType = DEVICE_SOURCE; // Default to device source (autovideosrc and autoaudiosrc)
   /* Initialize GStreamer */
   gst_init(&argc, &argv);
   DLOGI("[KVS Gstreamer Master] Finished initializing GStreamer and handlers");


   pSampleConfiguration->srcType = DEVICE_SOURCE;


   // Initalize KVS WebRTC. This must be done before anything else, and must only be done once.
   CHK_STATUS(initKvsWebRtc());
   DLOGI("[KVS GStreamer Master] KVS WebRTC initialization completed successfully");


   printf("main test 1\n");
   CHK_STATUS(initSignaling(pSampleConfiguration, SAMPLE_MASTER_CLIENT_ID));
   DLOGI("[KVS GStreamer Master] Channel %s set up done ", pChannelName);


   printf("main test 2\n");
   // Checking for termination
   CHK_STATUS(sessionCleanupWait(pSampleConfiguration));
   DLOGI("[KVS GStreamer Master] Streaming session terminated");


   printf("main test 3\n");
   goto CleanUp;


CleanUp:
   printf("main test 4\n");


   if (retStatus != STATUS_SUCCESS) {
       printf("[KVS GStreamer Master] Terminated with status\n");
   }


   printf("main test 4.5\n");
   DLOGI("main test DLOGI\n");
   DLOGI("[KVS GStreamer Master] Cleaning up...\n.");


   if (pSampleConfiguration != NULL) {
       printf("main test 5\n");
       // Kick of the termination sequence
       ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);


       printf("main test 5.1\n");
       /*if (pSampleConfiguration->mediaSenderTid != INVALID_TID_VALUE) {
           THREAD_JOIN(pSampleConfiguration->mediaSenderTid, NULL);
       }*/


       MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);


       printf("main test 5.2\n");
       if (pSampleConfiguration->enableFileLogging) {
           freeFileLogger();
       }
       printf("main test 5.3\n");
       retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
       printf("main test 5.4\n");
       if (retStatus != STATUS_SUCCESS) {
           printf("[KVS GStreamer Master] freeSignalingClient(): operation returned status code: 0x%08x\n", retStatus);
       }


       retStatus = freeSampleConfiguration(&pSampleConfiguration);
       printf("main test 5.5\n");
       if (retStatus != STATUS_SUCCESS) {
           printf("[KVS GStreamer Master] freeSampleConfiguration(): operation returned status code: 0x%08x\n", retStatus);
       }
   }
   DLOGI("[KVS Gstreamer Master] Cleanup done");
   printf("main test 6\n");
   RESET_INSTRUMENTED_ALLOCATORS();


   // https://www.gnu.org/software/libc/manual/html_node/Exit-Status.html
   // We can only return with 0 - 127. Some platforms treat exit code >= 128
   // to be a success code, which might give an unintended behaviour.
   // Some platforms also treat 1 or 0 differently, so it's better to use
   // EXIT_FAILURE and EXIT_SUCCESS macros for portability.
   return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}

