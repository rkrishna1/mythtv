// C headers
#include <cassert>
#include <unistd.h>

// C++ headers
#include <algorithm>
#include <iostream>
using namespace std;

// MythTV headers
#include "avformatdecoder.h"
#include "RingBuffer.h"
#include "NuppelVideoPlayer.h"
#include "remoteencoder.h"
#include "programinfo.h"
#include "mythcontext.h"
#include "mythdbcon.h"
#include "iso639.h"

#ifdef USING_XVMC
#include "videoout_xv.h"
extern "C" {
#include "libavcodec/xvmc_render.h"
}
#endif
extern "C" {
#include "libavcodec/liba52/a52.h"
#include "../libmythmpeg2/mpeg2.h"
}

#define LOC QString("AFD: ")
#define LOC_ERR QString("AFD Error: ")

#define MAX_AC3_FRAME_SIZE 6144

/** Set to zero to allow any number of AC3 channels. */
#define MAX_OUTPUT_CHANNELS 2

extern pthread_mutex_t avcodeclock;

int get_avf_buffer_xvmc(struct AVCodecContext *c, AVFrame *pic);
int get_avf_buffer(struct AVCodecContext *c, AVFrame *pic);
void release_avf_buffer(struct AVCodecContext *c, AVFrame *pic);
void release_avf_buffer_xvmc(struct AVCodecContext *c, AVFrame *pic);
void render_slice_xvmc(struct AVCodecContext *s, const AVFrame *src,
                       int offset[4], int y, int type, int height);

void align_dimensions(AVCodecContext *avctx, int &width, int &height)
{
    // minimum buffer alignment
    avcodec_align_dimensions(avctx, &width, &height);
    // minimum MPEG alignment
    width  = (width  + 15) & (~0xf);
    height = (height + 15) & (~0xf);
}

class AvFormatDecoderPrivate
{
  public:
    AvFormatDecoderPrivate() : mpeg2dec(NULL) { ; }
   ~AvFormatDecoderPrivate() { DestroyMPEG2(); }
    
    bool InitMPEG2();
    bool HasMPEG2Dec() const { return (bool)(mpeg2dec); }

    void DestroyMPEG2();
    void ResetMPEG2();
    int DecodeMPEG2Video(AVCodecContext *avctx, AVFrame *picture,
                         int *got_picture_ptr, uint8_t *buf, int buf_size);

  private:
    mpeg2dec_t *mpeg2dec;
};

bool AvFormatDecoderPrivate::InitMPEG2()
{
    DestroyMPEG2();
    QString dec = gContext->GetSetting("PreferredMPEG2Decoder", "ffmpeg");
    if (dec == "libmpeg2")
    {
        mpeg2dec = mpeg2_init();
        if (mpeg2dec)
            VERBOSE(VB_PLAYBACK, LOC + "Using libmpeg2 for video decoding");
    }
    return (mpeg2dec != NULL);
}

void AvFormatDecoderPrivate::DestroyMPEG2()
{
    if (mpeg2dec)
        mpeg2_close(mpeg2dec);
    mpeg2dec = NULL;
}

void AvFormatDecoderPrivate::ResetMPEG2()
{
    if (mpeg2dec)
        mpeg2_reset(mpeg2dec, 0);
}

int AvFormatDecoderPrivate::DecodeMPEG2Video(AVCodecContext *avctx,
                                             AVFrame *picture,
                                             int *got_picture_ptr,
                                             uint8_t *buf, int buf_size)
{
    *got_picture_ptr = 0;
    const mpeg2_info_t *info = mpeg2_info(mpeg2dec);
    mpeg2_buffer(mpeg2dec, buf, buf + buf_size);
    while (1)
    {
        switch (mpeg2_parse(mpeg2dec))
        {
            case STATE_SEQUENCE:
                // libmpeg2 needs three buffers to do its work.
                // We set up two prediction buffers here, from
                // the set of available video frames.
                mpeg2_custom_fbuf(mpeg2dec, 1);
                for (int i = 0; i < 2; i++)
                {
                    avctx->get_buffer(avctx, picture);
                    mpeg2_set_buf(mpeg2dec, picture->data, picture->opaque);
                }
                break;
            case STATE_PICTURE:
                // This sets up the third buffer for libmpeg2.
                // We use up one of the three buffers for each
                // frame shown. The frames get released once
                // they are drawn (outside this routine).
                avctx->get_buffer(avctx, picture);
                mpeg2_set_buf(mpeg2dec, picture->data, picture->opaque);
                break;
            case STATE_BUFFER:
                // We're supposed to wait for STATE_SLICE, but
                // STATE_BUFFER is returned first. Since we handed
                // libmpeg2 a full frame, we know it should be
                // done once it's finished with the data.
                if (info->display_fbuf)
                {
                    picture->data[0] = info->display_fbuf->buf[0];
                    picture->data[1] = info->display_fbuf->buf[1];
                    picture->data[2] = info->display_fbuf->buf[2];
                    picture->opaque  = info->display_fbuf->id;
                    *got_picture_ptr = 1;
                    picture->top_field_first = !!(info->display_picture->flags &
                                                  PIC_FLAG_TOP_FIELD_FIRST);
                    picture->interlaced_frame = !(info->display_picture->flags &
                                                  PIC_FLAG_PROGRESSIVE_FRAME);
                }
                return buf_size;
            case STATE_INVALID:
                // This is the error state. The decoder must be
                // reset on an error.
                mpeg2_reset(mpeg2dec, 0);
                return -1;
            default:
                break;
        }
    }
}

AvFormatDecoder::AvFormatDecoder(NuppelVideoPlayer *parent, ProgramInfo *pginfo,
                                 bool use_null_videoout)
    : DecoderBase(parent, pginfo),
      d(new AvFormatDecoderPrivate()), ic(NULL),
      frame_decoded(0), directrendering(false), drawband(false), bitrate(0),
      gopset(false), seen_gop(false), seq_count(0), firstgoppos(0),
      prevgoppos(0), gotvideo(false), lastvpts(0), lastapts(0),
      lastccptsu(0),
      using_null_videoout(use_null_videoout), video_codec_id(kCodec_NONE),
      maxkeyframedist(-1), 
      ccd(new CCDecoder(this)),
      // Audio
      audioSamples(new short int[AVCODEC_MAX_AUDIO_FRAME_SIZE]),
      allow_ac3_passthru(false),
      // Audio selection
      wantedAudioStream(),    selectedAudioStream(),
      // Subtitle selection
      wantedSubtitleStream(), selectedSubtitleStream(),
      // language preference
      languagePreference(iso639_get_language_key_list())
{
    bzero(&params, sizeof(AVFormatParameters));
    bzero(prvpkt, 3 * sizeof(char));
    bzero(audioSamples, AVCODEC_MAX_AUDIO_FRAME_SIZE * sizeof(short int));

    bool debug = (bool)(print_verbose_messages & VB_LIBAV);
    av_log_set_level((debug) ? AV_LOG_DEBUG : AV_LOG_ERROR);

    save_cctc[0] = save_cctc[1] = 0;
    allow_ac3_passthru = gContext->GetNumSetting("AC3PassThru", false);

    audioIn.sample_size = -32; // force SetupAudioStream to run once
}

AvFormatDecoder::~AvFormatDecoder()
{
    CloseContext();
    delete ccd;
    delete d;
    if (audioSamples)
        delete [] audioSamples;
}

void AvFormatDecoder::CloseContext()
{
    if (ic)
    {
        for (int i = 0; i < ic->nb_streams; i++)
        {
            AVStream *st = ic->streams[i];
            if (st->codec->codec)
                avcodec_close(st->codec);
        }

        ic->iformat->flags |= AVFMT_NOFILE;

        av_free(ic->pb.buffer);
        av_close_input_file(ic);
        ic = NULL;
    }
    d->DestroyMPEG2();
}

static int64_t lsb3full(int64_t lsb, int64_t base_ts, int lsb_bits)
{
    int64_t mask = (lsb_bits < 64) ? (1LL<<lsb_bits)-1 : -1LL;
    return  ((lsb - base_ts)&mask);
}

bool AvFormatDecoder::DoRewind(long long desiredFrame, bool doflush)
{
    VERBOSE(VB_PLAYBACK, LOC + "DoRewind("
            <<desiredFrame<<", "<<( doflush ? "do" : "don't" )<<" flush)");

    if (recordingHasPositionMap || livetv)
        return DecoderBase::DoRewind(desiredFrame, doflush);

    // avformat-based seeking
    return DoFastForward(desiredFrame, doflush);
}

bool AvFormatDecoder::DoFastForward(long long desiredFrame, bool doflush)
{
    VERBOSE(VB_PLAYBACK, LOC + "DoFastForward("
            <<desiredFrame<<", "<<( doflush ? "do" : "don't" )<<" flush)");

    if (recordingHasPositionMap || livetv)
        return DecoderBase::DoFastForward(desiredFrame, doflush);

    bool oldrawstate = getrawframes;
    getrawframes = false;

    AVStream *st = NULL;
    int i;
    for (i = 0; i < ic->nb_streams; i++)
    {
        AVStream *st1 = ic->streams[i];
        if (st1 && st1->codec->codec_type == CODEC_TYPE_VIDEO)
        {
            st = st1;
            break;
        }
    }
    if (!st)
        return false;

    int64_t frameseekadjust = 0;
    AVCodecContext *context = st->codec;

    if (context->codec_id == CODEC_ID_MPEG1VIDEO ||
        context->codec_id == CODEC_ID_MPEG2VIDEO ||
        context->codec_id == CODEC_ID_MPEG2VIDEO_XVMC ||
        context->codec_id == CODEC_ID_MPEG2VIDEO_XVMC_VLD)
    {
        frameseekadjust = maxkeyframedist+1;
    }

    // convert framenumber to normalized timestamp
    long double diff = (max(desiredFrame - frameseekadjust, 0LL)) * AV_TIME_BASE;
    long long ts = (long long)( diff / fps );
    if (av_seek_frame(ic, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR 
                <<"av_seek_frame(ic, -1, "<<ts<<", 0) -- error");
        return false;
    }

    int64_t adj_cur_dts = st->cur_dts;

    if (ic->start_time != (int64_t)AV_NOPTS_VALUE)
    {
        int64_t st1 = av_rescale(ic->start_time,
                                st->time_base.den,
                                AV_TIME_BASE * (int64_t)st->time_base.num);
        adj_cur_dts = lsb3full(adj_cur_dts, st1, st->pts_wrap_bits);
    }

    long long newts = av_rescale(adj_cur_dts,
                            (int64_t)AV_TIME_BASE * (int64_t)st->time_base.num,
                            st->time_base.den);

    // convert current timestamp to frame number
    lastKey = (long long)((newts*(long double)fps)/AV_TIME_BASE);
    framesPlayed = lastKey;
    framesRead = lastKey;

    int normalframes = desiredFrame - framesPlayed;

    SeekReset(lastKey, normalframes, doflush);

    if (doflush)
    {
        GetNVP()->SetFramesPlayed(framesPlayed + 1);
        GetNVP()->getVideoOutput()->SetFramesPlayed(framesPlayed + 1);
    }

    getrawframes = oldrawstate;

    return true;
}

void AvFormatDecoder::SeekReset(long long, int skipFrames, bool doflush)
{
    VERBOSE(VB_PLAYBACK, LOC + "SeekReset("
            <<skipFrames<<", "<<( doflush ? "do" : "don't" )<<" flush)");

    DecoderBase::SeekReset();

    lastapts = 0;
    lastvpts = 0;
    lastccptsu = 0;
    save_cctc[0] = save_cctc[1] = 0;

    av_read_frame_flush(ic);
    
    d->ResetMPEG2();

    // only reset the internal state if we're using our seeking, not libavformat's
    if (recordingHasPositionMap || livetv)
    {
        ic->pb.pos = ringBuffer->GetReadPosition();
        ic->pb.buf_ptr = ic->pb.buffer;
        ic->pb.buf_end = ic->pb.buffer;
        ic->pb.eof_reached = 0;
    }

    if (doflush)
    {
        VERBOSE(VB_PLAYBACK, LOC + "SeekReset() flushing");
        for (int i = 0; i < ic->nb_streams; i++)
        {
            AVCodecContext *enc = ic->streams[i]->codec;
            if (enc->codec)
                avcodec_flush_buffers(enc);
        }

        // TODO here we may need to wait for flushing to complete...

        GetNVP()->DiscardVideoFrames();
    }

    while (storedPackets.count() > 0)
    {
        AVPacket *pkt = storedPackets.first();
        storedPackets.removeFirst();
        av_free_packet(pkt);
        delete pkt;
    }

    prevgoppos = 0;
    gopset = false;

    while (skipFrames > 0)
    {
        GetFrame(0);
        GetNVP()->ReleaseNextVideoFrame();

        if (ateof)
            break;
        skipFrames--;
    }
}

void AvFormatDecoder::Reset(bool reset_video_data, bool seek_reset)
{
    VERBOSE(VB_PLAYBACK, LOC + QString("Reset(%1, %2)")
            .arg(reset_video_data).arg(seek_reset));
    if (seek_reset)
        SeekReset();

    if (reset_video_data)
    {
        m_positionMap.clear();
        framesPlayed = 0;
        framesRead = 0;
        seen_gop = false;
        seq_count = 0;
    }
}

void AvFormatDecoder::Reset()
{
    DecoderBase::Reset();
#if 0
// This is causing problems, and may not be needed anymore since
// we do not reuse the same file for different streams anymore. -- dtk

    // mpeg ts reset
    if (QString("mpegts") == ic->iformat->name)
    {
        AVInputFormat *fmt = (AVInputFormat*) av_mallocz(sizeof(AVInputFormat));
        memcpy(fmt, ic->iformat, sizeof(AVInputFormat));
        fmt->flags |= AVFMT_NOFILE;

        CloseContext();
        ic = av_alloc_format_context();
        if (!ic)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR +
                    "Reset(): Could not allocate format context.");
            av_free(fmt);
            errored = true;
            return;
        }

        InitByteContext();
        ic->streams_changed = HandleStreamChange;
        ic->stream_change_data = this;

        char *filename = (char *)(ringBuffer->GetFilename().ascii());
        int err = av_open_input_file(&ic, filename, fmt, 0, &params);
        if (err < 0)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "Reset(): "
                    "avformat err("<<err<<") on av_open_input_file call.");
            av_free(fmt);
            errored = true;
            return;
        }

        fmt->flags &= ~AVFMT_NOFILE;
    }
#endif
}

bool AvFormatDecoder::CanHandle(char testbuf[2048], const QString &filename)
{
    av_register_all();

    AVProbeData probe;

    probe.filename = (char *)(filename.ascii());
    probe.buf = (unsigned char *)testbuf;
    probe.buf_size = 2048;

    if (av_probe_input_format(&probe, true))
        return true;
    return false;
}

int open_avf(URLContext *h, const char *filename, int flags)
{
    (void)h;
    (void)filename;
    (void)flags;
    return 0;
}

int read_avf(URLContext *h, uint8_t *buf, int buf_size)
{
    AvFormatDecoder *dec = (AvFormatDecoder *)h->priv_data;

    return dec->getRingBuf()->Read(buf, buf_size);
}

int write_avf(URLContext *h, uint8_t *buf, int buf_size)
{
    (void)h;
    (void)buf;
    (void)buf_size;
    return 0;
}

offset_t seek_avf(URLContext *h, offset_t offset, int whence)
{
    AvFormatDecoder *dec = (AvFormatDecoder *)h->priv_data;

    if (whence == SEEK_END)
        return dec->getRingBuf()->GetRealFileSize() + offset;

    return dec->getRingBuf()->Seek(offset, whence);
}

int close_avf(URLContext *h)
{
    (void)h;
    return 0;
}

URLProtocol rbuffer_protocol = {
    "rbuffer",
    open_avf,
    read_avf,
    write_avf,
    seek_avf,
    close_avf,
    NULL
};

static int avf_write_packet(void *opaque, uint8_t *buf, int buf_size)
{
    URLContext *h = (URLContext *)opaque;
    return url_write(h, buf, buf_size);
}

static int avf_read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    URLContext *h = (URLContext *)opaque;
    return url_read(h, buf, buf_size);
}

static offset_t avf_seek_packet(void *opaque, int64_t offset, int whence)
{
    URLContext *h = (URLContext *)opaque;
    return url_seek(h, offset, whence);
}

void AvFormatDecoder::InitByteContext(void)
{
    readcontext.prot = &rbuffer_protocol;
    readcontext.flags = 0;
    readcontext.is_streamed = 0;
    readcontext.max_packet_size = 0;
    readcontext.priv_data = this;

    ic->pb.buffer_size = 32768;
    ic->pb.buffer = (unsigned char *)av_malloc(ic->pb.buffer_size);
    ic->pb.buf_ptr = ic->pb.buffer;
    ic->pb.write_flag = 0;
    ic->pb.buf_end = ic->pb.buffer;
    ic->pb.opaque = &readcontext;
    ic->pb.read_packet = avf_read_packet;
    ic->pb.write_packet = avf_write_packet;
    ic->pb.seek = avf_seek_packet;
    ic->pb.pos = 0;
    ic->pb.must_flush = 0;
    ic->pb.eof_reached = 0;
    ic->pb.is_streamed = 0;
    ic->pb.max_packet_size = 0;
}

extern "C" void HandleStreamChange(void* data) {
    AvFormatDecoder* decoder = (AvFormatDecoder*) data;
    int cnt = decoder->ic->nb_streams;

    VERBOSE(VB_PLAYBACK, LOC + "HandleStreamChange(): "
            "streams_changed "<<data<<" -- stream count "<<cnt);

    pthread_mutex_lock(&avcodeclock);
    decoder->SeekReset();
    decoder->ScanStreams(false);
    pthread_mutex_unlock(&avcodeclock);
}

/** \fn AvFormatDecoder::OpenFile(RingBuffer*, bool, char[2048])
 *  OpenFile opens a ringbuffer for playback.
 *
 *  OpenFile deletes any existing context then use testbuf to
 *  guess at the stream type. It then calls ScanStreams to find
 *  any valid streams to decode. If possible a position map is
 *  also built for quick skipping.
 *
 *  /param rbuffer pointer to a valid ringuffer.
 *  /param novideo if true then no video is sought in ScanSreams.
 *  /param _testbuf this paramater is not used by AvFormatDecoder.
 */
int AvFormatDecoder::OpenFile(RingBuffer *rbuffer, bool novideo, char testbuf[2048])
{
    CloseContext();

    ringBuffer = rbuffer;

    AVInputFormat *fmt = NULL;
    char *filename = (char *)(rbuffer->GetFilename().ascii());

    AVProbeData probe;
    probe.filename = filename;
    probe.buf = (unsigned char *)testbuf;
    probe.buf_size = 2048;

    fmt = av_probe_input_format(&probe, true);
    if (!fmt)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("Probe failed for file: \"%1\".").arg(filename));
        return -1;
    }

    fmt->flags |= AVFMT_NOFILE;

    ic = av_alloc_format_context();
    if (!ic)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                "Could not allocate format context.");
        return -1;
    }

    InitByteContext();

    int err = av_open_input_file(&ic, filename, fmt, 0, &params);
    if (err < 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR
                <<"avformat err("<<err<<") on av_open_input_file call.");
        return -1;
    }

    int ret = av_find_stream_info(ic);
    if (ret < 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Could not find codec parameters. " +
                QString("file was \"%1\".").arg(filename));
        av_close_input_file(ic);
        ic = NULL;
        return -1;
    }
    ic->streams_changed = HandleStreamChange;
    ic->stream_change_data = this;

    fmt->flags &= ~AVFMT_NOFILE;

    av_estimate_timings(ic);

    // Scan for the initial A/V streams
    ret = ScanStreams(novideo);
    if (-1 == ret)
        return ret;

    // Select some starting audio and subtitle tracks.
    // TODO do we need this? They will be called by GetFrame() anyway...
    autoSelectAudioTrack();
    autoSelectSubtitleTrack();

    // Try to get a position map from the recorder if we don't have one yet.
    if (!recordingHasPositionMap)
    {
        if ((m_playbackinfo) || livetv || watchingrecording)
        {
            recordingHasPositionMap |= SyncPositionMap();
            if (recordingHasPositionMap && !livetv && !watchingrecording)
            {
                hasFullPositionMap = true;
                gopset = true;
            }
        }
    }

    // If we don't have a position map, set up ffmpeg for seeking
    if (!recordingHasPositionMap)
    {
        VERBOSE(VB_PLAYBACK, LOC +
                "Recording has no position -- using libavformat seeking.");
        int64_t dur = ic->duration / (int64_t)AV_TIME_BASE;

        if (dur > 0)
        {
            GetNVP()->SetFileLength((int)(dur), (int)(dur * fps));
        }
        else
        {
            // the pvr-250 seems to overreport the bitrate by * 2
            float bytespersec = (float)bitrate / 8 / 2;
            float secs = ringBuffer->GetRealFileSize() * 1.0 / bytespersec;
            GetNVP()->SetFileLength((int)(secs), (int)(secs * fps));
        }

        // we will not see a position map from db or remote encoder,
        // set the gop interval to 15 frames.  if we guess wrong, the
        // auto detection will change it.
        keyframedist = 15;
        positionMapType = MARK_GOP_START;

        if (!strcmp(fmt->name, "avi"))
        {
            // avi keyframes are too irregular
            keyframedist = 1;
            positionMapType = MARK_GOP_BYFRAME;
        }
    }

    // Don't build a seek index for MythTV files, the user needs to
    // use mythcommflag to build a proper MythTV position map for these.
    if (livetv || watchingrecording)
        ic->build_index = 0;

    dump_format(ic, 0, filename, 0);

    // print some useful information if playback debugging is on
    if (hasFullPositionMap)
        VERBOSE(VB_PLAYBACK, LOC + "Position map found");
    else if (recordingHasPositionMap)
        VERBOSE(VB_PLAYBACK, LOC + "Partial position map found");
    VERBOSE(VB_PLAYBACK, LOC +
            QString("Successfully opened decoder for file: "
                    "\"%1\". novideo(%2)").arg(filename).arg(novideo));

    // Return true if recording has position map
    return recordingHasPositionMap;
}

static float normalized_fps(AVCodecContext *enc)
{
    float fps = 1.0f / av_q2d(enc->time_base);

    // Some formats report fps waaay too high. (wrong time_base)
    if (fps > 121.0f && (enc->time_base.den > 10000) &&
        (enc->time_base.num == 1))
    {
        enc->time_base.num = 1001;  // seems pretty standard
        if (av_q2d(enc->time_base) > 0)
            fps = 1.0f / av_q2d(enc->time_base);
    }
    // If it is still out of range, just assume NTSC...
    fps = (fps > 121.0f) ? (30000.0f / 1001.0f) : fps;
    return fps;
}

void AvFormatDecoder::InitVideoCodec(AVCodecContext *enc)
{
    fps = normalized_fps(enc);

    float aspect_ratio;
    if (enc->sample_aspect_ratio.num == 0)
        aspect_ratio = 0.0f;
    else
        aspect_ratio = av_q2d(enc->sample_aspect_ratio) *
            enc->width / enc->height;

    if (aspect_ratio <= 0.0f)
        aspect_ratio = (float)enc->width / (float)enc->height;

    current_width = enc->width;
    current_height = enc->height;
    current_aspect = aspect_ratio;

    enc->opaque = NULL;
    enc->get_buffer = NULL;
    enc->release_buffer = NULL;
    enc->draw_horiz_band = NULL;
    enc->slice_flags = 0;

    enc->error_resilience = FF_ER_COMPLIANT;
    enc->workaround_bugs = FF_BUG_AUTODETECT;
    enc->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    enc->idct_algo = FF_IDCT_AUTO;
    enc->debug = 0;
    enc->rate_emu = 0;
    enc->error_rate = 0;

    AVCodec *codec = avcodec_find_decoder(enc->codec_id);    

    if (!gContext->GetNumSetting("DecodeExtraAudio", 0))
        SetLowBuffers(false);

    if (codec && (codec->id == CODEC_ID_MPEG2VIDEO_XVMC ||
                  codec->id == CODEC_ID_MPEG2VIDEO_XVMC_VLD))
    {
        enc->flags |= CODEC_FLAG_EMU_EDGE;
        enc->opaque = (void *)this;
        enc->get_buffer = get_avf_buffer_xvmc;
        enc->release_buffer = release_avf_buffer_xvmc;
        enc->draw_horiz_band = render_slice_xvmc;
        enc->slice_flags = SLICE_FLAG_CODED_ORDER |
            SLICE_FLAG_ALLOW_FIELD;
        directrendering = true;
    }
    else if (codec && codec->capabilities & CODEC_CAP_DR1 &&
             !(enc->width % 16))
    {
        enc->flags |= CODEC_FLAG_EMU_EDGE;
        enc->opaque = (void *)this;
        enc->get_buffer = get_avf_buffer;
        enc->release_buffer = release_avf_buffer;
        enc->draw_horiz_band = NULL;
        directrendering = true;
    }

    int align_width = enc->width;
    int align_height = enc->height;

    align_dimensions(enc, align_width, align_height);

    if (align_width == 0 && align_height == 0)
    {
        align_width = 640;
        align_height = 480;
        fps = 29.97;
        aspect_ratio = 4.0 / 3;
    }

    GetNVP()->SetVideoParams(align_width, align_height, fps,
                             keyframedist, aspect_ratio, kScan_Detect);
}

#ifdef USING_XVMC
static int xvmc_stream_type(int codec_id)
{
    switch (codec_id)
    {
        case CODEC_ID_MPEG1VIDEO:
            return 1;
        case CODEC_ID_MPEG2VIDEO:
        case CODEC_ID_MPEG2VIDEO_XVMC:
        case CODEC_ID_MPEG2VIDEO_XVMC_VLD:
            return 2;
#if 0
// We don't support these yet.
        case CODEC_ID_H263:
            return 3;
        case CODEC_ID_MPEG4:
            return 4;
#endif
    }
    return 0;
}

static int xvmc_pixel_format(enum PixelFormat pix_fmt)
{
    (void) pix_fmt;
    int xvmc_chroma = XVMC_CHROMA_FORMAT_420;
#if 0
// We don't support other chromas yet
    if (PIX_FMT_YUV420P == pix_fmt)
        xvmc_chroma = XVMC_CHROMA_FORMAT_420;
    else if (PIX_FMT_YUV422P == pix_fmt)
        xvmc_chroma = XVMC_CHROMA_FORMAT_422;
    else if (PIX_FMT_YUV420P == pix_fmt)
        xvmc_chroma = XVMC_CHROMA_FORMAT_444;
#endif
    return xvmc_chroma;
}
#endif // USING_XVMC

int AvFormatDecoder::ScanStreams(bool novideo)
{
    int scanerror = 0;
    bitrate = 0;
    fps = 0;

    audioStreams.clear();
    subtitleStreams.clear();

    map<int,uint> lang_sub_cnt;
    map<int,uint> lang_aud_cnt;

    for (int i = 0; i < ic->nb_streams; i++)
    {
        AVCodecContext *enc = ic->streams[i]->codec;
        VERBOSE(VB_PLAYBACK, LOC +
                QString("Stream #%1, has id 0x%2 codec id %3, type %4 at 0x")
                .arg(i).arg((int)ic->streams[i]->id)
                .arg(codec_id_string(enc->codec_id))
                .arg(codec_type_string(enc->codec_type))
                <<((void*)ic->streams[i]));

        switch (enc->codec_type)
        {
            case CODEC_TYPE_VIDEO:
            {
                assert(enc->codec_id);
                bitrate += enc->bit_rate;
                if (novideo)
                    break;

                d->DestroyMPEG2();
#ifdef USING_XVMC
                if (!using_null_videoout && xvmc_stream_type(enc->codec_id))
                {
                    // HACK -- begin
                    // Force MPEG2 decoder on MPEG1 streams.
                    // Needed for broken transmitters which mark
                    // MPEG2 streams as MPEG1 streams, and should
                    // be harmless for unbroken ones.
                    if (CODEC_ID_MPEG1VIDEO == enc->codec_id)
                        enc->codec_id = CODEC_ID_MPEG2VIDEO;
                    // HACK -- end

                    MythCodecID mcid;
                    mcid = VideoOutputXv::GetBestSupportedCodec(
                        /* disp dim     */ enc->width, enc->height,
                        /* osd dim      */ /*enc->width*/ 0, /*enc->height*/ 0,
                        /* mpeg type    */ xvmc_stream_type(enc->codec_id),
                        /* xvmc pix fmt */ xvmc_pixel_format(enc->pix_fmt),
                        /* test surface */ kCodec_NORMAL_END > video_codec_id);
                    bool vcd, idct, mc;
                    enc->codec_id = myth2av_codecid(mcid, vcd, idct, mc);
                    video_codec_id = mcid;
                    if (kCodec_NORMAL_END < mcid && kCodec_STD_XVMC_END > mcid)
                    {
                        enc->pix_fmt = (idct) ?
                            PIX_FMT_XVMC_MPEG2_IDCT : PIX_FMT_XVMC_MPEG2_MC;
                    }
                }
                else
                    video_codec_id = kCodec_MPEG2; // default to MPEG2
#else
                video_codec_id = kCodec_MPEG2; // default to MPEG2
#endif // USING_XVMC
                if (enc->codec)
                {
                    VERBOSE(VB_IMPORTANT, LOC
                            <<"Warning, video codec "<<enc<<" "
                            <<"id("<<codec_id_string(enc->codec_id)
                            <<") type ("<<codec_type_string(enc->codec_type)
                            <<") already open.");
                }
                InitVideoCodec(enc);
                // Only use libmpeg2 when not using XvMC
                if (CODEC_ID_MPEG1VIDEO == enc->codec_id ||
                    CODEC_ID_MPEG2VIDEO == enc->codec_id)
                {
                    d->InitMPEG2();
                }

                enc->decode_cc_dvd  = decode_cc_dvd;
                enc->decode_cc_atsc = decode_cc_atsc;
                break;
            }
            case CODEC_TYPE_AUDIO:
            {
                if (enc->codec)
                {
                    VERBOSE(VB_IMPORTANT, LOC
                            <<"Warning, audio codec "<<enc
                            <<" id("<<codec_id_string(enc->codec_id)
                            <<") type ("<<codec_type_string(enc->codec_type)
                            <<") already open, leaving it alone.");
                }
                assert(enc->codec_id);
                bitrate += enc->bit_rate;
                break;
            }
            case CODEC_TYPE_SUBTITLE:
            {
                bitrate += enc->bit_rate;
                VERBOSE(VB_PLAYBACK, LOC + QString("subtitle codec (%1)")
                        .arg(codec_type_string(enc->codec_type)));
                break;
            }
            case CODEC_TYPE_DATA:
            {
                bitrate += enc->bit_rate;
                VERBOSE(VB_PLAYBACK, LOC + QString("data codec, ignoring (%1)")
                        .arg(codec_type_string(enc->codec_type)));
                break;
            }
            default:
            {
                bitrate += enc->bit_rate;
                VERBOSE(VB_PLAYBACK, LOC + QString("Unknown codec type (%1)")
                        .arg(codec_type_string(enc->codec_type)));
                break;
            }
        }

        if (enc->codec_type != CODEC_TYPE_AUDIO && 
            enc->codec_type != CODEC_TYPE_VIDEO &&
            enc->codec_type != CODEC_TYPE_SUBTITLE)
            continue;

        VERBOSE(VB_PLAYBACK, LOC + QString("Looking for decoder for %1")
                .arg(codec_id_string(enc->codec_id)));
        AVCodec *codec = avcodec_find_decoder(enc->codec_id);
        if (!codec)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + 
                    QString("Could not find decoder for "
                            "codec (%1), ignoring.")
                    .arg(codec_id_string(enc->codec_id)));
            continue;
        }

        if (!enc->codec)
        {
            int open_val = avcodec_open(enc, codec);
            if (open_val < 0)
            {
                VERBOSE(VB_IMPORTANT, LOC_ERR
                        <<"Could not open codec "<<enc<<", "
                        <<"id("<<codec_id_string(enc->codec_id)<<") "
                        <<"type("<<codec_type_string(enc->codec_type)<<") "
                        <<"aborting. reason "<<open_val);
                //av_close_input_file(ic); // causes segfault
                ic = NULL;
                scanerror = -1;
                break;
            }
            else
            {
                VERBOSE(VB_GENERAL, LOC + "Opened codec "<<enc<<", "
                        <<"id("<<codec_id_string(enc->codec_id)<<") "
                        <<"type("<<codec_type_string(enc->codec_type)<<")");
            }
        }

        if (enc->codec_type == CODEC_TYPE_SUBTITLE)
        {
            int lang = -1, lang_indx = 0;
            if (ic->streams[i]->language)
            {
                lang = iso639_str3_to_key(ic->streams[i]->language);
                lang = iso639_key_to_canonical_key(lang);
                lang_indx = lang_sub_cnt[lang];
                lang_sub_cnt[lang]++;
            }
            subtitleStreams.push_back(StreamInfo(i, lang, lang_indx));

            VERBOSE(VB_PLAYBACK, LOC + QString(
                        "Subtitle track #%1 is A/V stream #%2 "
                        "and is in the %3 language(%4).")
                    .arg(subtitleStreams.size()).arg(i)
                    .arg(iso639_key_toName(lang)).arg(lang));
        }

        if (enc->codec_type == CODEC_TYPE_AUDIO)
        {
            int lang = -1, lang_indx = 0;
            if (ic->streams[i]->language)
            {
                lang = iso639_str3_to_key(ic->streams[i]->language);
                lang = iso639_key_to_canonical_key(lang);
                lang_indx = lang_aud_cnt[lang];
                lang_aud_cnt[lang]++;
            }
            audioStreams.push_back(StreamInfo(i, lang, lang_indx));

            VERBOSE(VB_AUDIO, LOC + QString(
                        "Audio Track #%1 is A/V stream #%2 "
                        "and has %3 channels in the %4 language(%5).")
                    .arg(audioStreams.size()).arg(i)
                    .arg(enc->channels)
                    .arg(iso639_key_toName(lang)).arg(lang));
        }
    }

    if (bitrate > 0)
    {
        bitrate /= 1000;
        if (ringBuffer)
            ringBuffer->CalcReadAheadThresh(bitrate);
    }

    // Select a new track at the next opportunity.
    currentAudioTrack = -1;
    currentSubtitleTrack = -1;

    // We have to do this here to avoid the NVP getting stuck
    // waiting on audio.
    if (GetNVP()->HasAudioIn() && audioStreams.empty())
    {
        GetNVP()->SetAudioParams(-1, -1, -1);
        GetNVP()->ReinitAudio();
    }

    if (GetNVP()->IsErrored())
        scanerror = -1;

    return scanerror;
}

bool AvFormatDecoder::CheckVideoParams(int width, int height)
{
    if (width == current_width && height == current_height)
        return false;

    if (current_width && current_height)
    {
        VERBOSE(VB_GENERAL, LOC +
                QString("Video has changed from %1x%2 to %3x%4 pixels.")
                .arg(current_width).arg(current_height)
                .arg(width).arg(height));
    }
    else
    {
        VERBOSE(VB_GENERAL, LOC +
                QString("Initializing video at %1x%2 pixels.")
                .arg(width).arg(height));
    }   

    for (int i = 0; i < ic->nb_streams; i++)
    {
        AVCodecContext *enc = ic->streams[i]->codec;
        switch (enc->codec_type)
        {
            case CODEC_TYPE_VIDEO:
            {
                AVCodec *codec = enc->codec;
                if (!codec) {
                    VERBOSE(VB_IMPORTANT, LOC_ERR + 
                            QString("Codec for stream %1 is null").arg(i));
                    break;
                }
                break;
            }
            default:
                break;
        }
    }

    return true;
}

int get_avf_buffer(struct AVCodecContext *c, AVFrame *pic)
{
    AvFormatDecoder *nd = (AvFormatDecoder *)(c->opaque);

    VideoFrame *frame = nd->GetNVP()->GetNextVideoFrame(true);

    int width = frame->width;
    int height = frame->height;

    pic->data[0] = frame->buf;
    pic->data[1] = pic->data[0] + width * height;
    pic->data[2] = pic->data[1] + width * height / 4;

    pic->linesize[0] = width;
    pic->linesize[1] = width / 2;
    pic->linesize[2] = width / 2;

    pic->opaque = frame;
    pic->type = FF_BUFFER_TYPE_USER;

    pic->age = 256 * 256 * 256 * 64;

    return 1;
}

void release_avf_buffer(struct AVCodecContext *c, AVFrame *pic)
{
    (void)c;

    if (pic->type == FF_BUFFER_TYPE_INTERNAL)
    {
        avcodec_default_release_buffer(c, pic);
        return;
    }

    assert(pic->type == FF_BUFFER_TYPE_USER);

    int i;
    for (i = 0; i < 4; i++)
        pic->data[i] = NULL;
}

int get_avf_buffer_xvmc(struct AVCodecContext *c, AVFrame *pic)
{
    AvFormatDecoder *nd = (AvFormatDecoder *)(c->opaque);
    VideoFrame *frame = nd->GetNVP()->GetNextVideoFrame(false);

    pic->data[0] = frame->priv[0];
    pic->data[1] = frame->priv[1];
    pic->data[2] = frame->buf;

    pic->linesize[0] = 0;
    pic->linesize[1] = 0;
    pic->linesize[2] = 0;

    pic->opaque = frame;
    pic->type = FF_BUFFER_TYPE_USER;

    pic->age = 256 * 256 * 256 * 64;

#ifdef USING_XVMC
    xvmc_render_state_t *render = (xvmc_render_state_t *)frame->buf;

    render->state = MP_XVMC_STATE_PREDICTION;
    render->picture_structure = 0;
    render->flags = 0;
    render->start_mv_blocks_num = 0;
    render->filled_mv_blocks_num = 0;
    render->next_free_data_block_num = 0;
#endif

    return 1;
}

void release_avf_buffer_xvmc(struct AVCodecContext *c, AVFrame *pic)
{
    (void)c;

    assert(pic->type == FF_BUFFER_TYPE_USER);

#ifdef USING_XVMC
    xvmc_render_state_t *render = (xvmc_render_state_t *)pic->data[2];
    render->state &= ~MP_XVMC_STATE_PREDICTION;
#endif

    int i;
    for (i = 0; i < 4; i++)
        pic->data[i] = NULL;

}

void render_slice_xvmc(struct AVCodecContext *s, const AVFrame *src,
                       int offset[4], int y, int type, int height)
{
    if (!src)
        return;

    (void)offset;
    (void)type;

    if (s && src && s->opaque && src->opaque)
    {
        AvFormatDecoder *nd = (AvFormatDecoder *)(s->opaque);

        int width = s->width;

        VideoFrame *frame = (VideoFrame *)src->opaque;
        nd->GetNVP()->DrawSlice(frame, 0, y, width, height);
    }
    else
        cerr<<"render_slice_xvmc called with bad avctx or src"<<endl;
}

void decode_cc_dvd(struct AVCodecContext *s, const uint8_t *buf, int buf_size)
{
    // taken from xine-lib libspucc by Christian Vogler

    AvFormatDecoder *nd = (AvFormatDecoder *)(s->opaque);
    unsigned long long utc = nd->lastccptsu;

    const uint8_t *current = buf;
    int curbytes = 0;
    uint8_t data1, data2;
    uint8_t cc_code;
    int odd_offset = 1;

    while (curbytes < buf_size)
    {
        int skip = 2;

        cc_code = *current++;
        curbytes++;
    
        if (buf_size - curbytes < 2)
            break;
    
        data1 = *current;
        data2 = *(current + 1);
    
        switch (cc_code)
        {
            case 0xfe:
                /* expect 2 byte encoding (perhaps CC3, CC4?) */
                /* ignore for time being */
                skip = 2;
                break;

            case 0xff:
            {
                /* expect EIA-608 CC1/CC2 encoding */
                int tc = utc / 1000;
                int data = (data2 << 8) | data1;
                nd->ccd->FormatCCField(tc, 0, data);
                utc += 33367;
                skip = 5;
                break;
            }

            case 0x00:
                /* This seems to be just padding */
                skip = 2;
                break;

            case 0x01:
                odd_offset = data2 & 0x80;
                if (odd_offset)
                    skip = 2;
                else
                    skip = 5;
                break;

            default:
                // rest is not needed?
                goto done;
                //skip = 2;
                //break;
        }
        current += skip;
        curbytes += skip;

    }
  done:
    nd->lastccptsu = utc;
}

void decode_cc_atsc(struct AVCodecContext *s, const uint8_t *buf, int buf_size)
{
    AvFormatDecoder *nd = (AvFormatDecoder *)(s->opaque);
    unsigned long long utc = nd->lastccptsu;

    const uint8_t *current = buf;
    int curbytes = 0;
    int curcount = 0;
    uint8_t data1, data2;
    uint8_t cc_count;
    uint8_t cc_code;
    int  cc_state;

    if (buf_size < 2)
        return;

    // check process_cc_data_flag
    if (!(*current & 0x40))
        return;
    cc_count = *current & 0x1f;
    current++;  curbytes++;

    // skip em_data
    current++;  curbytes++;

    cc_state = 0;
    while (curbytes < buf_size && curcount < cc_count)
    {
        cc_code = *current++;
        curbytes++;
    
        if (buf_size - curbytes < 2)
            break;
    
        data1 = *current++;
        data2 = *current++;
        curbytes += 2;
        curcount++;

        // check cc_valid
        if (!(cc_code & 0x04))
            continue;

        cc_code &= 0x03;
        if (cc_code <= 0x1)
        {
            // EIA-608 field-1/2
            int data = (data2 << 8) | data1;
            unsigned int tc;

            if ((cc_state & cc_code) == cc_code)
            {
                // another field of same type -- assume
                // it's for the next frame
                utc += 33367;
                cc_state = 0;
            }
            cc_state |= cc_code;
            tc = utc / 1000;

            // For some reason, one frame can be out of order.
            // We need to save the CC codes for at least one
            // frame so we can send the correct sequence to the
            // decoder.

            if (nd->save_cctc[cc_code])
            {
                if (nd->save_cctc[cc_code] < tc)
                {
                    // send saved code to decoder
                    nd->ccd->FormatCCField(nd->save_cctc[cc_code],
                                           cc_code,
                                           nd->save_ccdata[cc_code]);
                    nd->save_cctc[cc_code] = 0;
                }
                else if ((nd->save_cctc[cc_code] - tc) > 1000)
                {
                    // saved code is too far in the future; probably bad
                    // - discard it
                    nd->save_cctc[cc_code] = 0;
                }
                else
                {
                    // send new code to decoder
                    nd->ccd->FormatCCField(tc, cc_code, data);
                }
            }
            if (!nd->save_cctc[cc_code])
            {
                // no saved code
                // - save new code since it may be out of order
                nd->save_cctc[cc_code] = tc;
                nd->save_ccdata[cc_code] = data;
            }
        }
        else
        {
            // TODO:  EIA-708 DTVCC packet data
        }

    }
    nd->lastccptsu = utc;
}

void AvFormatDecoder::HandleGopStart(AVPacket *pkt)
{
    if (prevgoppos != 0 && keyframedist != 1)
    {
        int tempKeyFrameDist = framesRead - 1 - prevgoppos;

        if (!gopset) // gopset: we've seen 2 keyframes
        {
            VERBOSE(VB_PLAYBACK, LOC + "HandleGopStart: "
                    "gopset not set, syncing positionMap");
            SyncPositionMap();
            if (tempKeyFrameDist > 0)
            {
                gopset = true;
                keyframedist = tempKeyFrameDist;
                if (keyframedist > maxkeyframedist)
                    maxkeyframedist = keyframedist;

                if ((keyframedist == 15) ||
                    (keyframedist == 12))
                    positionMapType = MARK_GOP_START;
                else
                    positionMapType = MARK_GOP_BYFRAME;

                VERBOSE(VB_PLAYBACK, LOC + "HandleGopStart: " +
                        QString("Initial key frame distance: %1.")
                        .arg(keyframedist));

                GetNVP()->SetKeyframeDistance(keyframedist);
            }
        }
        else
        {
            if (keyframedist != tempKeyFrameDist && tempKeyFrameDist > 0)
            {
                VERBOSE(VB_PLAYBACK, LOC + "HandleGopStart: " +
                        QString("Key frame distance changed from %1 to %2.")
                        .arg(keyframedist).arg(tempKeyFrameDist));

                keyframedist = tempKeyFrameDist;
                if (keyframedist > maxkeyframedist)
                    maxkeyframedist = keyframedist;

                if ((keyframedist == 15) ||
                    (keyframedist == 12))
                    positionMapType = MARK_GOP_START;
                else
                    positionMapType = MARK_GOP_BYFRAME;

                GetNVP()->SetKeyframeDistance(keyframedist);

#if 0
                // also reset length
                long long index = m_positionMap[m_positionMap.size() - 1].index;
                long long totframes = index * keyframedist;
                int length = (int)((totframes * 1.0) / fps);

                VERBOSE(VB_PLAYBACK, LOC + "HandleGopStart: "
                        QString("Setting file length to %1 seconds, "
                                "with %2 frames total.")
                        .arg(length).arg((int)totframes));
                GetNVP()->SetFileLength(length, totframes);
#endif
            }
        }
    }

    lastKey = prevgoppos = framesRead - 1;

    if (!hasFullPositionMap)
    {
        long long last_frame = 0;
        if (!m_positionMap.empty())
            last_frame = m_positionMap[m_positionMap.size() - 1].index;
        if (keyframedist > 1)
            last_frame *= keyframedist;

        //cerr << "framesRead: " << framesRead << " last_frame: " << last_frame
        //    << " keyframedist: " << keyframedist << endl;

        // if we don't have an entry, fill it in with what we've just parsed
        if (framesRead > last_frame && keyframedist > 0)
        {
            long long startpos = pkt->pos;

            VERBOSE(VB_PLAYBACK, LOC + 
                    QString("positionMap[ %1 ] == %2.")
                    .arg(prevgoppos / keyframedist)
                    .arg((int)startpos));

            // Grow positionMap vector several entries at a time
            if (m_positionMap.capacity() == m_positionMap.size())
                m_positionMap.reserve(m_positionMap.size() + 60);
            PosMapEntry entry = {prevgoppos / keyframedist,
                                 prevgoppos, startpos};
            m_positionMap.push_back(entry);
        }

#if 0
        // If we are > 150 frames in and saw no positionmap at all, reset
        // length based on the actual bitrate seen so far
        if (framesRead > 150 && !recordingHasPositionMap && !livetv)
        {
            bitrate = (int)((pkt->pos * 8 * fps) / (framesRead - 1));
            float bytespersec = (float)bitrate / 8;
            float secs = ringBuffer->GetRealFileSize() * 1.0 / bytespersec;
            GetNVP()->SetFileLength((int)(secs), (int)(secs * fps));
        }
#endif
    }
}

#define SEQ_START     0x000001b3
#define GOP_START     0x000001b8
#define PICTURE_START 0x00000100
#define SLICE_MIN     0x00000101
#define SLICE_MAX     0x000001af

void AvFormatDecoder::MpegPreProcessPkt(AVStream *stream, AVPacket *pkt)
{
    AVCodecContext *context = stream->codec;
    unsigned char *bufptr = pkt->data;
    //unsigned char *bufend = pkt->data + pkt->size;
    unsigned int state = 0xFFFFFFFF, v = 0;
    int prvcount = -1;

    while (bufptr < pkt->data + pkt->size)
    {
        if (++prvcount < 3)
            v = prvpkt[prvcount];
        else
            v = *bufptr++;

        if (state == 0x000001)
        {
            state = ((state << 8) | v) & 0xFFFFFF;
            if (state >= SLICE_MIN && state <= SLICE_MAX)
                continue;

            switch (state)
            {
                case SEQ_START:
                {
                    unsigned char *test = bufptr;
                    int width = (test[0] << 4) | (test[1] >> 4);
                    int height = ((test[1] & 0xff) << 8) | test[2];

                    int aspectratioinfo = (test[3] >> 4);

                    float aspect = GetMpegAspect(context, aspectratioinfo,
                                                 width, height);

                    if (width < 2500 && height < 2000 &&
                        (CheckVideoParams(width, height) ||
                         aspect != current_aspect))
                    {
                        int align_width = width;
                        int align_height = height;
                        align_dimensions(context, align_width, align_height);

                        GetNVP()->SetVideoParams(align_width, align_height,
                                                 normalized_fps(context),
                                                 keyframedist, aspect, 
                                                 kScan_Detect, true);
                        current_width = width;
                        current_height = height;
                        current_aspect = aspect;

                        d->ResetMPEG2();

                        gopset = false;
                        prevgoppos = 0;
                        lastapts = lastvpts = lastccptsu = 0;
                    }

                    seq_count++;

                    if (!seen_gop && seq_count > 1)
                        HandleGopStart(pkt);
                }
                break;

                case GOP_START:
                {
                    HandleGopStart(pkt);
                    seen_gop = true;
                }
                break;
            }
            continue;
        }
        state = ((state << 8) | v) & 0xFFFFFF;
    }

    memcpy(prvpkt, pkt->data + pkt->size - 3, 3);
}

static const float avfmpeg1_aspect[16]={
    0.0000,    1.0000,    0.6735,    0.7031,
    0.7615,    0.8055,    0.8437,    0.8935,
    0.9157,    0.9815,    1.0255,    1.0695,
    1.0950,    1.1575,    1.2015,
};

static const float avfmpeg2_aspect[16]={
    0,    1.0,    -3.0/4.0,    -9.0/16.0,    -1.0/2.21,
};

float AvFormatDecoder::GetMpegAspect(AVCodecContext *context,
                                     int aspect_ratio_info,
                                     int width, int height)
{
    float retval = 0;

    if (aspect_ratio_info > 15)
        aspect_ratio_info = 15;
    if (aspect_ratio_info < 0)
        aspect_ratio_info = 0;

    if (context->sub_id == 1) // mpeg1
    {
        float aspect = avfmpeg1_aspect[aspect_ratio_info];
        if (aspect != 0.0)
            retval = width / (aspect * height);
    }
    else
    {
        float aspect = avfmpeg2_aspect[aspect_ratio_info];
        if (aspect > 0)
            retval = width / (aspect * height);
        else if (aspect < 0)
            retval = -1.0 / aspect;
    }

    if (retval <= 0)
        retval = width * 1.0 / height;

    return retval;
}

void AvFormatDecoder::incCurrentAudioTrack(void)
{
    int numStreams = (int)audioStreams.size();
    int next       = (currentAudioTrack+1) % numStreams;
    setCurrentAudioTrack((!numStreams) ? -1 : next);
}

void AvFormatDecoder::decCurrentAudioTrack(void)
{
    int numStreams = (int)audioStreams.size();
    int next       = (currentAudioTrack < 0) ? 0 : currentAudioTrack;
    next           = (next+numStreams-1) % numStreams;
    setCurrentAudioTrack((!numStreams) ? -1 : next);
}

bool AvFormatDecoder::setCurrentAudioTrack(int trackNo)
{
    if (trackNo >= (int)audioStreams.size())
        return false;

    pthread_mutex_lock(&avcodeclock);

    currentAudioTrack = max(-1, trackNo);
    if (currentAudioTrack >= 0)
    {
        wantedAudioStream   = audioStreams[currentAudioTrack];
        selectedAudioStream = audioStreams[currentAudioTrack];
    }
    else
        selectedAudioStream.av_stream_index = -1;

    QString msg = SetupAudioStream() ? "" : "not ";
    VERBOSE(VB_AUDIO, LOC + "Audio stream type "+msg+"changed.");

    pthread_mutex_unlock(&avcodeclock);

    return (currentAudioTrack >= 0);
}

QStringList AvFormatDecoder::listAudioTracks(void) const
{
    QStringList list;

    for (uint i = 0; i < audioStreams.size(); i++)
    {
        QString msg  = iso639_key_toName(audioStreams[i].language);
        AVStream *s  = ic->streams[audioStreams[i].av_stream_index];
        if (s)
        {
            if (s->codec->codec_id == CODEC_ID_MP3)
                msg += QString(" MP%1").arg(s->codec->sub_id);
            else
                msg += QString(" %1").arg(s->codec->codec->name).upper();

            if (!s->codec->channels)
                msg += QString(" ?ch");
            else if ((s->codec->channels > 4) && !(s->codec->channels & 1))
                msg += QString(" %1.1ch").arg(s->codec->channels - 1);
            else
                msg += QString(" %1ch").arg(s->codec->channels);
        }
        list += QString("%1: %2").arg(i+1).arg(msg);
    }

    return list;
}

vector<int> filter_lang(const sinfo_vec_t &audioStreams, int lang_key)
{
    vector<int> ret;

    for (uint i=0; i<audioStreams.size(); i++)
        if ((lang_key < 0) || audioStreams[i].language == lang_key)
            ret.push_back(i);

    return ret;
}

int filter_ac3(const AVFormatContext *ic,
               const sinfo_vec_t     &audioStreams,
               const vector<int>     &fs)
{
    int selectedTrack = -1;

    vector<int>::const_iterator it = fs.begin();
    for (; it != fs.end(); ++it)
    {
        const int stream_index    = audioStreams[*it].av_stream_index;
        const AVCodecContext *ctx = ic->streams[stream_index]->codec;
        if (CODEC_ID_AC3 == ctx->codec_id)
        {
            selectedTrack = *it;
            break;
        }
    }

    return selectedTrack;
}

int filter_max_ch(const AVFormatContext *ic,
                  const sinfo_vec_t     &audioStreams,
                  const vector<int>     &fs)
{
    int selectedTrack = -1, max_seen = -1;

    vector<int>::const_iterator it = fs.begin();
    for (; it != fs.end(); ++it)
    {
        const int stream_index    = audioStreams[*it].av_stream_index;
        const AVCodecContext *ctx = ic->streams[stream_index]->codec;
        if (max_seen < ctx->channels)
        {
            selectedTrack = *it;
            max_seen = ctx->channels;
        }
    }

    return selectedTrack;
}

/** \fn AvFormatDecoder::autoSelectAudioTrack(void)
 *  \brief Selects the best audio track.
 *
 *   This function will select the best audio track
 *   available using the following rules:
 *
 *   First we try to select the stream last selected by the
 *   user, which is recalled as the Nth stream in the 
 *   preferred language. If it can not be located we attempt
 *   to find a stream in the same language.
 *
 *   If we can not reselect the last user selected stream,
 *   then for each preferred language from most preferred to
 *   least preferred, we try to use the first AC3 track found,
 *   or if no AC3 audio is found then we try to select the
 *   audio track with the greatest number of audio channels.
 *
 *   If we can not select a stream in a preferred language
 *   we try to use the first AC3 track found irrespective
 *   of language, and if no AC3 audio is found then we try
 *   to select the audio track with the greatest number of
 *   audio channels.
 *  \return true if a track was selected, false otherwise
 */
bool AvFormatDecoder::autoSelectAudioTrack(void)
{
    uint numStreams = audioStreams.size();
    if ((currentAudioTrack >= 0) && (currentAudioTrack < (int)numStreams))
        return true; // audio already selected

#if 0
    // enable this to print streams
    for (uint i=0; i<audioStreams.size(); i++)
    {
        int idx = audioStreams[i].av_stream_index;
        AVCodecContext *codec_ctx = ic->streams[idx]->codec;
        AudioInfo item(codec_ctx->codec_id,
                       codec_ctx->sample_rate, codec_ctx->channels,
                       (allow_ac3_passthru && !transcoding &&
                        (codec_ctx->codec_id == CODEC_ID_AC3)));
        VERBOSE(VB_AUDIO, LOC + " * " + item.toString());
    }
#endif

    int selectedTrack = (1 == numStreams) ? 0 : -1;

    if ((selectedTrack < 0) && wantedAudioStream.language>=-1 && numStreams)
    {
        VERBOSE(VB_AUDIO, LOC + "Trying to reselect audio track");
        // Try to reselect user selected subtitle stream.
        // This should find the stream after a commercial
        // break and in some cases after a channel change.
        int  wlang = wantedAudioStream.language;
        uint windx = wantedAudioStream.language_index;
        for (uint i = 0; i < numStreams; i++)
        {
            if (wlang == audioStreams[i].language)
                selectedTrack = i;
            if (windx == audioStreams[i].language_index)
                break;
        }
    }

    if (selectedTrack < 0 && numStreams)
    {
        VERBOSE(VB_AUDIO, LOC + "Trying to select audio track (w/lang)");
        // try to get best track for most preferred language
        selectedTrack = -1;
        vector<int>::const_iterator it = languagePreference.begin();
        for (; it !=  languagePreference.end(); ++it)
        {
            vector<int> flang = filter_lang(audioStreams, *it);
            if ((selectedTrack = filter_ac3(ic, audioStreams, flang)) >= 0)
                break;
            if ((selectedTrack = filter_max_ch(ic, audioStreams, flang)) >= 0)
                break;
        }
        // try to get best track for any language
        if (selectedTrack < 0)
        {
            VERBOSE(VB_AUDIO, LOC + "Trying to select audio track (wo/lang)");
            vector<int> flang = filter_lang(audioStreams, -1);
            if ((selectedTrack = filter_ac3(ic, audioStreams, flang)) < 0)
                selectedTrack  = filter_max_ch(ic, audioStreams, flang);
        }
    }

    if (selectedTrack < 0)
    {
        selectedAudioStream.av_stream_index = -1;
        if (currentAudioTrack != selectedTrack)
        {
            VERBOSE(VB_AUDIO, LOC + "No suitable audio track exists.");
            currentAudioTrack = selectedTrack;
        }
    }
    else
    {
        currentAudioTrack     = selectedTrack;
        selectedAudioStream   = audioStreams[currentAudioTrack];
        if (wantedAudioStream.av_stream_index < 0)
            wantedAudioStream = selectedAudioStream;

        if (print_verbose_messages & VB_AUDIO)
        {
            QStringList list = listAudioTracks();
            VERBOSE(VB_AUDIO, LOC +
                    QString("Selected track %1 (A/V Stream #%2)")
                    .arg(list[currentAudioTrack])
                    .arg(selectedAudioStream.av_stream_index));
        }
    }

    SetupAudioStream();
    return selectedTrack >= 0;
}

void AvFormatDecoder::incCurrentSubtitleTrack(void)
{
    int numStreams = (int)subtitleStreams.size();
    int next       = (currentSubtitleTrack+1) % numStreams;
    setCurrentSubtitleTrack((!numStreams) ? -1 : next);
}

void AvFormatDecoder::decCurrentSubtitleTrack(void)
{
    int numStreams = (int)subtitleStreams.size();
    int next       = max(currentSubtitleTrack, 0);
    next           = (next+numStreams-1) % numStreams;
    setCurrentSubtitleTrack((!numStreams) ? -1 : next);
}

bool AvFormatDecoder::setCurrentSubtitleTrack(int trackNo)
{
    if (trackNo >= (int)subtitleStreams.size())
        return false;

    pthread_mutex_lock(&avcodeclock);

    currentSubtitleTrack = max(-1, trackNo);
    if (currentSubtitleTrack >= 0)
    {
        wantedSubtitleStream   = subtitleStreams[currentSubtitleTrack];
        selectedSubtitleStream = subtitleStreams[currentSubtitleTrack];
    }
    else
        selectedSubtitleStream.av_stream_index = -1;

    pthread_mutex_unlock(&avcodeclock);

    return currentSubtitleTrack >= 0;
}

QStringList AvFormatDecoder::listSubtitleTracks(void) const
{
    QStringList list;

    for (uint i = 0; i < subtitleStreams.size(); i++)
    {
        QString msg  = iso639_key_toName(subtitleStreams[i].language);
        list += QString("%1: %2").arg(i+1).arg(msg);
    }

    return list;
}

/** \fn AvFormatDecoder::autoSelectSubtitleTrack(void)
 *  \brief Select best subtitle track.
 *
 *   If case there's only one subtitle available, always choose it.
 *
 *   If there is a user selected subtitle we try to find it.
 *
 *   If we can't find the user selected subtitle we try to
 *   picked according to the ISO639Language[0..] settings.
 *
 *   In case there are no ISOLanguage[0..] settings, or no preferred language
 *   is found, the first found subtitle stream is chosen
 *  \return true if a track was selected, false otherwise
 */
bool AvFormatDecoder::autoSelectSubtitleTrack(void)
{
    uint numStreams = subtitleStreams.size();

    if ((currentSubtitleTrack >= 0) && 
        (currentSubtitleTrack < (int)numStreams))
    {
        return true; // subtitle already selected
    }

    if (!numStreams)
    {
        currentSubtitleTrack = -1;
        selectedSubtitleStream.av_stream_index = -1;
        return false; // no subtitles available
    }

    int selectedTrack = (1 == numStreams) ? 0 : -1;

    if ((selectedTrack < 0) && wantedSubtitleStream.language>=-1 && numStreams)
    {
        VERBOSE(VB_PLAYBACK, LOC + "Trying to reselect subtitle track");
        // Try to reselect user selected subtitle stream.
        // This should find the stream after a commercial
        // break and in some cases after a channel change.
        int  wlang = wantedSubtitleStream.language;
        uint windx = wantedSubtitleStream.language_index;
        for (uint i = 0; i < numStreams; i++)
        {
            if (wlang == subtitleStreams[i].language)
                selectedTrack = i;
            if (windx == subtitleStreams[i].language_index)
                break;
        }
    }

    if (selectedTrack < 0 && numStreams)
    {
        VERBOSE(VB_PLAYBACK, LOC + "Trying to select subtitle track (w/lang)");
        // Find first subtitle stream that matches a language in
        // order of most preferred to least preferred language.
        vector<int>::iterator it = languagePreference.begin();
        for (; it != languagePreference.end(); ++it)
        {
            for (uint i = 0; i < numStreams; i++)
            {
                if (*it == subtitleStreams[i].language)
                {
                    selectedTrack = i;
                    break;
                }
            }
        }
    }

    currentSubtitleTrack = (selectedTrack < 0) ? 0 : -1;
    selectedSubtitleStream = subtitleStreams[currentSubtitleTrack];
    if (wantedSubtitleStream.av_stream_index < 0)
        wantedSubtitleStream = selectedSubtitleStream;
     
    int lang = subtitleStreams[currentSubtitleTrack].language;
    VERBOSE(VB_PLAYBACK, LOC + 
            QString("Selected subtitle track #%1 in the %2 language(%3)")
            .arg(currentSubtitleTrack+1)
            .arg(iso639_key_toName(lang)).arg(lang));

    return true;
}

bool AvFormatDecoder::GetFrame(int onlyvideo)
{
    AVPacket *pkt = NULL;
    int len, ret = 0;
    unsigned char *ptr;
    int data_size = 0;
    long long pts;
    bool firstloop = false, have_err = false;

    gotvideo = false;

    frame_decoded = 0;

    bool allowedquit = false;
    bool storevideoframes = false;

    pthread_mutex_lock(&avcodeclock);
    autoSelectAudioTrack();
    autoSelectSubtitleTrack();
    pthread_mutex_unlock(&avcodeclock);

    bool skipaudio = (lastvpts == 0);

    while (!allowedquit)
    {
        if ((onlyvideo == 0) &&
            ((currentAudioTrack<0) || (selectedAudioStream.av_stream_index<0)))
        {
            // disable audio request if there are no audio streams anymore
            onlyvideo = 1;
        }

        if (gotvideo)
        {
            if (lowbuffers && onlyvideo == 0 && lastapts < lastvpts + 100 &&
                lastapts > lastvpts - 10000)
            {
                //cout << "behind: " << lastapts << " " << lastvpts << endl;
                storevideoframes = true;
            }
            else
            {
                allowedquit = true;
                continue;
            }
        }

        if (!storevideoframes && storedPackets.count() > 0)
        {
            if (pkt)
            {
                av_free_packet(pkt);
                delete pkt;
            }
            pkt = storedPackets.first();
            storedPackets.removeFirst();
        }
        else
        {
            if (!pkt)
            {
                pkt = new AVPacket;
                bzero(pkt, sizeof(AVPacket));
                av_init_packet(pkt);
            }

            if (!ic || (av_read_frame(ic, pkt) < 0))
            {
                ateof = true;
                GetNVP()->SetEof();
                return false;
            }

            if (waitingForChange && pkt->pos >= readAdjust)
                FileChanged();

            if (pkt->pos > readAdjust)
                pkt->pos -= readAdjust;
        }

        if (pkt->stream_index > ic->nb_streams)
        {
            cerr << "bad stream\n";
            av_free_packet(pkt);
            continue;
        }

        len = pkt->size;
        ptr = pkt->data;
        pts = 0;

        AVStream *curstream = ic->streams[pkt->stream_index];

        if (pkt->dts != (int64_t)AV_NOPTS_VALUE)
            pts = (long long)(av_q2d(curstream->time_base) * pkt->dts * 1000);

        if (storevideoframes &&
            curstream->codec->codec_type == CODEC_TYPE_VIDEO)
        {
            av_dup_packet(pkt);
            storedPackets.append(pkt);
            pkt = NULL;
            continue;
        }

        if (len > 0 && curstream->codec->codec_type == CODEC_TYPE_VIDEO)
        {
            if (framesRead == 0 && !justAfterChange && 
                !(pkt->flags & PKT_FLAG_KEY))
            {
                av_free_packet(pkt);
                continue;
            }

            framesRead++;
            justAfterChange = false;

            if (exitafterdecoded)
                gotvideo = 1;

            AVCodecContext *context = curstream->codec;

            if (context->codec_id == CODEC_ID_MPEG1VIDEO ||
                context->codec_id == CODEC_ID_MPEG2VIDEO ||
                context->codec_id == CODEC_ID_MPEG2VIDEO_XVMC ||
                context->codec_id == CODEC_ID_MPEG2VIDEO_XVMC_VLD)
            {
                MpegPreProcessPkt(curstream, pkt);
            }
            else
            {
                if (pkt->flags & PKT_FLAG_KEY)
                {
                    HandleGopStart(pkt);
                    seen_gop = true;
                }
                else
                {
                    seq_count++;
                    if (!seen_gop && seq_count > 1)
                    {
                        HandleGopStart(pkt);
                    }
                }
            }
        }

        if (!curstream->codec->codec)
        {
            VERBOSE(VB_PLAYBACK, LOC + QString("No codec for stream index %1")
                    .arg(pkt->stream_index));
            av_free_packet(pkt);
            continue;
        }

        firstloop = true;
        have_err = false;

        pthread_mutex_lock(&avcodeclock);
        int ctype  = curstream->codec->codec_type;
        int audIdx = selectedAudioStream.av_stream_index;
        int subIdx = selectedSubtitleStream.av_stream_index;
        pthread_mutex_unlock(&avcodeclock);

        while (!have_err && len > 0)
        {
            switch (ctype)
            {
                case CODEC_TYPE_AUDIO:
                {
                    // detect channels on streams that need
                    // to be decoded before we can know this
                    if (!curstream->codec->channels)
                    {
                        pthread_mutex_lock(&avcodeclock);
                        curstream->codec->channels = MAX_OUTPUT_CHANNELS;
                        ret = avcodec_decode_audio(
                            curstream->codec, audioSamples,
                            &data_size, ptr, len);
                        if (curstream->codec->channels)
                        {
                            currentAudioTrack = -1;
                            selectedAudioStream.av_stream_index = -1;
                            audIdx = -1;
                            autoSelectAudioTrack();
                            audIdx = selectedAudioStream.av_stream_index;
                        }
                        pthread_mutex_unlock(&avcodeclock);
                    }

                    if (firstloop && pkt->pts != (int64_t)AV_NOPTS_VALUE)
                        lastapts = (long long)(av_q2d(curstream->time_base) *
                                               pkt->pts * 1000);

                    if (onlyvideo != 0 || (pkt->stream_index != audIdx))
                    {
                        ptr += len;
                        len = 0;
                        continue;
                    }

                    if (skipaudio)
                    {
                        if (lastapts < lastvpts - 30 || lastvpts == 0)
                        {
                            ptr += len;
                            len = 0;
                            continue;
                        }
                        else
                            skipaudio = false;
                    }

                    pthread_mutex_lock(&avcodeclock);
                    ret = len;
                    data_size = 0;
                    if (audioOut.do_ac3_passthru)
                    {
                        data_size = pkt->size;
                        ret = EncodeAC3Frame(ptr, len, audioSamples,
                                             data_size);
                    }
                    else
                    {
                        AVCodecContext *ctx = curstream->codec;

                        if ((ctx->channels == 0) ||
                            (ctx->channels > MAX_OUTPUT_CHANNELS))
                            ctx->channels = MAX_OUTPUT_CHANNELS;

                        ret = avcodec_decode_audio(
                            ctx, audioSamples, &data_size, ptr, len);

                        // When decoding some audio streams the number of
                        // channels, etc isn't known until we try decoding it.
                        if ((ctx->sample_rate != audioOut.sample_rate) ||
                            (ctx->channels    != audioOut.channels))
                        {
                            VERBOSE(VB_IMPORTANT, "audio stream changed");
                            currentAudioTrack = -1;
                            selectedAudioStream.av_stream_index = -1;
                            audIdx = -1;
                            autoSelectAudioTrack();
                            data_size = 0;
                        }
                    }
                    pthread_mutex_unlock(&avcodeclock);

                    ptr += ret;
                    len -= ret;

                    if (data_size <= 0)
                        continue;

                    long long temppts = lastapts;

                    // calc for next frame
                    lastapts += (long long)((double)(data_size * 1000) /
                                (curstream->codec->channels * 2) / 
                                curstream->codec->sample_rate);

                    GetNVP()->AddAudioData((char *)audioSamples, data_size,
                                           temppts);

                    break;
                }
                case CODEC_TYPE_VIDEO:
                {
                    if (firstloop && pts != (int64_t) AV_NOPTS_VALUE)
                    {
                        lastccptsu = (long long)
                            (av_q2d(curstream->time_base)*pkt->pts*1000000);
                    }
                    if (onlyvideo < 0)
                    {
                        framesPlayed++;
                        ptr += pkt->size;
                        len -= pkt->size;
                        continue;
                    }

                    AVCodecContext *context = curstream->codec;
                    AVFrame mpa_pic;
                    bzero(&mpa_pic, sizeof(AVFrame));

                    int gotpicture = 0;

                    pthread_mutex_lock(&avcodeclock);
                    if (d->HasMPEG2Dec())
                        ret = d->DecodeMPEG2Video(context, &mpa_pic,
                                                  &gotpicture, ptr, len);
                    else
                        ret = avcodec_decode_video(context, &mpa_pic,
                                                   &gotpicture, ptr, len);
                    pthread_mutex_unlock(&avcodeclock);

                    if (ret < 0)
                    {
                        cerr << "decoding error\n";
                        have_err = true;
                        continue;
                    }

                    if (!gotpicture)
                    {
                        ptr += ret;
                        len -= ret;
                        continue;
                    }

                    VideoFrame *picframe = (VideoFrame *)(mpa_pic.opaque);

                    if (!directrendering)
                    {
                        AVPicture tmppicture;
 
                        picframe = GetNVP()->GetNextVideoFrame();

                        tmppicture.data[0] = picframe->buf;
                        tmppicture.data[1] = tmppicture.data[0] + 
                                        picframe->width * picframe->height;
                        tmppicture.data[2] = tmppicture.data[1] + 
                                        picframe->width * picframe->height / 4;

                        tmppicture.linesize[0] = picframe->width;
                        tmppicture.linesize[1] = picframe->width / 2;
                        tmppicture.linesize[2] = picframe->width / 2;

                        img_convert(&tmppicture, PIX_FMT_YUV420P, 
                                    (AVPicture *)&mpa_pic,
                                    context->pix_fmt,
                                    context->width,
                                    context->height);
                    }

                    long long temppts = pts;

                    if (temppts != 0)
                        lastvpts = temppts;
                    else
                        temppts = lastvpts;

/* XXX: Broken.
                    if (mpa_pic.qscale_table != NULL && mpa_pic.qstride > 0 &&
                        context->height == picframe->height)
                    {
                        int tblsize = mpa_pic.qstride *
                                      ((picframe->height + 15) / 16);

                        if (picframe->qstride != mpa_pic.qstride ||
                            picframe->qscale_table == NULL)
                        {
                            picframe->qstride = mpa_pic.qstride;
                            if (picframe->qscale_table)
                                delete [] picframe->qscale_table;
                            picframe->qscale_table = new unsigned char[tblsize];
                        }

                        memcpy(picframe->qscale_table, mpa_pic.qscale_table,
                               tblsize);
                    }
*/

                    picframe->interlaced_frame = mpa_pic.interlaced_frame;
                    picframe->top_field_first = mpa_pic.top_field_first;

                    picframe->frameNumber = framesPlayed;
                    GetNVP()->ReleaseNextVideoFrame(picframe, temppts);
                    if (!directrendering)
                        GetNVP()->DiscardVideoFrame(picframe);

                    gotvideo = 1;
                    framesPlayed++;

                    lastvpts = temppts;
                    break;
                }
                case CODEC_TYPE_SUBTITLE:
                {
                    int gotSubtitles = 0;
                    AVSubtitle subtitle;

                    if (pkt->stream_index == subIdx)
                    {
                        pthread_mutex_lock(&avcodeclock);
                        avcodec_decode_subtitle(curstream->codec,
                                                &subtitle, &gotSubtitles,
                                                ptr, len);
                        pthread_mutex_unlock(&avcodeclock);
                    }

                    // the subtitle decoder always consumes the whole packet
                    ptr += len;
                    len = 0;

                    if (gotSubtitles) 
                    {
                        subtitle.start_display_time += pts;
                        subtitle.end_display_time += pts;
                        GetNVP()->AddSubtitle(subtitle);
                    }

                    break;
                }
                default:
                    cerr << "error decoding - " << curstream->codec->codec_type
                         << endl;
                    have_err = true;
                    break;
            }

            if (!have_err)
            {
                ptr += ret;
                len -= ret;
                frame_decoded = 1;
                firstloop = false;
            }
        }

        av_free_packet(pkt);
    }

    if (pkt)
        delete pkt;

    return true;
}

/** \fn AvFormatDecoder::SetupAudioStream(void)
 *  \brief Reinitializes audio if it needs to be reinitialized.
 *
 *   NOTE: The avcodeclock must be held when this is called.
 *
 *  \return true if audio changed, false otherwise
 */
bool AvFormatDecoder::SetupAudioStream(void)
{
    AudioInfo info; // no_audio
    AVStream *curstream = NULL;
    AVCodecContext *codec_ctx = NULL;
    AudioInfo old_in  = audioIn;
    AudioInfo old_out = audioOut;

    if ((currentAudioTrack >= 0) &&
        (selectedAudioStream.av_stream_index <= ic->nb_streams) &&
        (curstream = ic->streams[selectedAudioStream.av_stream_index]))
    {
        assert(curstream);
        assert(curstream->codec);
        codec_ctx = curstream->codec;        
        bool do_ac3_passthru = (allow_ac3_passthru && !transcoding &&
                                (codec_ctx->codec_id == CODEC_ID_AC3));
        info = AudioInfo(codec_ctx->codec_id,
                         codec_ctx->sample_rate, codec_ctx->channels,
                         do_ac3_passthru);
    }

    if (info == audioIn)
        return false; // no change

    VERBOSE(VB_AUDIO, LOC + "Initializing audio parms from " +
            QString("audio track #%1").arg(currentAudioTrack+1));

    audioOut = audioIn = info;
    if (audioIn.do_ac3_passthru)
    {
        // A passthru stream looks like a 48KHz 2ch (@ 16bit) to the sound card
        audioOut.channels    = 2;
        audioOut.sample_rate = 48000;
        audioOut.sample_size = 4;
    }
    else
    {
        if (audioOut.channels > MAX_OUTPUT_CHANNELS)
        {
            audioOut.channels = MAX_OUTPUT_CHANNELS;
            audioOut.sample_size = audioOut.channels * 2;
            codec_ctx->channels = MAX_OUTPUT_CHANNELS;
        }
    }

    VERBOSE(VB_AUDIO, LOC + "Audio format changed " +
            QString("\n\t\t\tfrom %1 ; %2\n\t\t\tto   %3 ; %4")
            .arg(old_in.toString()).arg(old_out.toString())
            .arg(audioIn.toString()).arg(audioOut.toString()));

    if (audioOut.sample_rate > 0)
        GetNVP()->SetEffDsp(audioOut.sample_rate * 100);

    GetNVP()->SetAudioParams(audioOut.bps(), audioOut.channels,
                             audioOut.sample_rate);
    GetNVP()->ReinitAudio();

    return true;
}

int AvFormatDecoder::EncodeAC3Frame(unsigned char *data, int len,
                                    short *samples, int &samples_size)
{
    int enc_len;
    int flags, sample_rate, bit_rate;
    unsigned char* ucsamples = (unsigned char*) samples;

    // we don't do any length/crc validation of the AC3 frame here; presumably
    // the receiver will have enough sense to do that.  if someone has a
    // receiver that doesn't, here would be a good place to put in a call
    // to a52_crc16_block(samples+2, data_size-2) - but what do we do if the
    // packet is bad?  we'd need to send something that the receiver would
    // ignore, and if so, may as well just assume that it will ignore
    // anything with a bad CRC...

    enc_len = a52_syncinfo(data, &flags, &sample_rate, &bit_rate);

    if (enc_len == 0 || enc_len > len)
    {
        samples_size = 0;
        return len;
    }

    if (enc_len > MAX_AC3_FRAME_SIZE - 8)
        enc_len = MAX_AC3_FRAME_SIZE - 8;

    swab(data, ucsamples + 8, enc_len);

    // the following values come from ao_hwac3.c in mplayer.
    // they form a valid IEC958 AC3 header.
    ucsamples[0] = 0x72;
    ucsamples[1] = 0xF8;
    ucsamples[2] = 0x1F;
    ucsamples[3] = 0x4E;
    ucsamples[4] = 0x01;
    ucsamples[5] = 0x00;
    ucsamples[6] = (enc_len << 3) & 0xFF;
    ucsamples[7] = (enc_len >> 5) & 0xFF;
    memset(ucsamples + 8 + enc_len, 0, MAX_AC3_FRAME_SIZE - 8 - enc_len);
    samples_size = MAX_AC3_FRAME_SIZE;

    return len;  // consume whole frame even if len > enc_len ?
}

void AvFormatDecoder::AddTextData(unsigned char *buf, int len,
                                  long long timecode, char type)
{
    m_parent->AddTextData((char*)buf, len, timecode, type);
}
