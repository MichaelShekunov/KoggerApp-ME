#include "id_binnary.h"
#include "math.h"
#include <QDateTime>
#include <utility>

// The codec is kept byte-identical to the copy the device runs, so it carries
// no C++ linkage guard of its own and gets one here instead. Do not "fix" this
// by editing kec.h: the two copies drifting apart is the failure this avoids.
extern "C" {
#include "kec.h"
}

#include <core.h>
extern Core core;

#if defined(Q_OS_ANDROID) || (defined Q_OS_LINUX)
template < typename T, size_t N >
size_t _countof( T const (&array)[ N ] ) { Q_UNUSED(array); return N; }
#endif

QString boardVersionName(BoardVersion version, uint8_t versionMinor)
{
    switch (version) {
    case BoardNone:
        if (versionMinor == BoardAssist)
            return QStringLiteral("Assist");
        break;
    case BoardEnhanced:
    case BoardNEnhanced:
        return QStringLiteral("2D-Enhanced");
    case BoardChirp:
        return QStringLiteral("2D-Chirp");
    case BoardBase:
    case BoardNBase:
        return QStringLiteral("2D-Base");
    case BoardAssist:
    case BoardRecorderMini:
        return QStringLiteral("Recorder");
    case BoardSideEnhanced:
        return QStringLiteral("Side-Enhanced");
    case BoardDVL:
        return QStringLiteral("DVL");
    case BoardBasic2D:
        return QStringLiteral("Basic2D");
    case BoardNanoSSS:
        return QStringLiteral("NanoSSS");
    case BoardUSBL:
        return QStringLiteral("USBL");
    case BoardUSBLBeacon:
        return QStringLiteral("Beacon");
    case BoardPULSEred_2D:
        return QStringLiteral("PULSEred 2D DSP");
    case BoardPULSEblue_DSS:
        return QStringLiteral("PULSEblue DSS");
    }
    return QStringLiteral("Device ID: %1.%2").arg(static_cast<int>(version)).arg(versionMinor);
}
IDBin::IDBin(QObject *parent) :
    QObject(parent),
    setTimerCount_(0),
    coldStartTimerCount_(0),
    isColdStart_(false), // for unique
    needToCheckSetResp_(true)
{
    qRegisterMetaType<ProtoBinOut>("ProtoBinOut");
    qRegisterMetaType<Parsers::ProtoBinOut>("Parsers::ProtoBinOut");
//    setProto(proto);

    coldStartTimer_.setInterval(timerPeriodMsec_);
    setTimer_.setInterval(timerPeriodMsec_);

    QObject::connect(&coldStartTimer_, &QTimer::timeout, this, &IDBin::onExpiredColdStartTimer);
    QObject::connect(&setTimer_, &QTimer::timeout, this, &IDBin::onExpiredSetTimer);
}

IDBin::~IDBin()
{
}

Resp  IDBin::parse(FrameParser &proto) {
    Resp resp_parse = respNone;

    if(proto.type() == CONTENT || proto.type() == SETTING || proto.type() == GETTING) {
        m_lastType = proto.type();
        m_lastVersion = proto.ver();
        _lastAddress = proto.route();

        if ((proto.resp() && proto.type() == CONTENT) ||
            proto.type() == GETTING) {
            m_lastResp = (Resp)proto.read<U1>();

            if ((m_lastResp == Resp::respOk)  && (!hashLastInfo_.isReaded && needToCheckSetResp_)) {
                if (checkResponse(proto)) {
                    hashLastInfo_.isReaded = true;
                    onExpiredSetTimer();
                }
                else {
                    needToCheckSetResp_ = false;
                }
            }

            resp_parse = respOk;
        }
        else if ((!proto.resp() && proto.type() == CONTENT) ||
                   proto.type() == SETTING) {

            m_lastResp = respNone;
            resp_parse = Resp::respOk;
            if (proto.type() == CONTENT || proto.type() == SETTING) {
                resp_parse = parsePayload(proto);
            }

            if (!hashLastInfo_.isReaded && !needToCheckSetResp_) {
                hashLastInfo_.isReaded = true;
                onExpiredSetTimer();
            }

            if (resp_parse == Resp::respOk && isColdStart_) {
                isColdStart_ = false;
                onExpiredColdStartTimer();
            }
        }

        if (resp_parse == respOk)
            emit updateContent(m_lastType, m_lastVersion, m_lastResp, _lastAddress);
    }

    return resp_parse;
}

bool IDBin::checkResponse(FrameParser& proto)
{
    auto checkSum = proto.read<U2>();

    Version ver = proto.ver();
    return (hashLastInfo_.checkSum == checkSum) &&
           (hashLastInfo_.address == proto.route()) &&
           (hashLastInfo_.version == ver);
}

void IDBin::simpleRequest(Version ver) {
    ProtoBinOut req_out;
    req_out.create(GETTING, ver, id(), m_address);
    requestSpecific(req_out);
    req_out.end();

    emit binFrameOut(req_out);
}

void IDBin::appendKey(ProtoBinOut &proto_out) {
    proto_out.write<U4>(m_key);
}

void IDBin::hashBinFrameOut(ProtoBinOut &proto_out)
{
    hashLastInfo_ = { proto_out.ver(), proto_out.checksum(), proto_out.route(), false };

    emit binFrameOut(proto_out); // to device

    if (setTimer_.isActive()) // restart timer
        setTimer_.stop();
    setTimer_.start();
}

void IDBin::interExecColdStartTimer()
{
    isColdStart_ = true;

    coldStartTimer_.start();
}

void IDBin::onExpiredColdStartTimer()
{
    coldStartTimer_.stop();

    emit notifyDevDriver(!isColdStart_);
    if (isColdStart_ && (coldStartTimerCount_++ < repeatingCount_)) { // try request
        requestAll();
        interExecColdStartTimer();
    }
    else {
        coldStartTimerCount_ = 0;
    }
}

void IDBin::onExpiredSetTimer()
{        
    setTimer_.stop();

    emit notifyDevDriver(hashLastInfo_.isReaded);
    if (!hashLastInfo_.isReaded && (setTimerCount_++ < repeatingCount_)) { // try request
        requestAll();

        setTimer_.start();
    }
    else {
        setTimerCount_ = 0;
        needToCheckSetResp_ = true;
    }
}

//void IDBin::sendDataProcessing(ProtoBinOut &proto_out) {
//    QByteArray data((char*)proto_out.frame(), proto_out.frameLen());
//    dataSend(data);
//    if(isConsoleOut) { core.consoleProto(proto_out); }
//}


Resp IDBinTimestamp::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {  m_timestamp = proto.read<U4>();
    } else {  return respErrorVersion;  }
    return respOk;
}

Resp IDBinDist::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) { m_dist_mm = proto.read<U4>();
    } else {  return respErrorVersion; }
    return respOk;
}

Resp IDBinChartZ::parsePayload(FrameParser &proto) {
    // v0 is one channel, v1 means the stream holds both sides end to end -- the same meaning the
    // version carries on an uncompressed ID_CHART.
    if (proto.ver() != v0 && proto.ver() != v1) {
        return respErrorVersion;
    }
    const int channels = (proto.ver() == v1) ? 2 : 1;

    const U2 seqOffset = proto.read<U2>();
    const U2 resol     = proto.read<U2>();
    const U2 absOffset = proto.read<U2>();
    const U2 samples   = proto.read<U2>();
    const U2 total     = proto.read<U2>();

    if (total == 0 || samples == 0) {
        return respErrorPayload;
    }

    // A slice at offset zero starts a ping; so does any change in the ping's
    // shape, which means the previous one was cut short.
    if (seqOffset == 0 || total != expected_ || channels != channels_ ||
        resol != resol_ || absOffset != absOffset_ || samples != samples_) {
        if (expected_ != 0 && filled_ < expected_) {
            ++dropped_;
        }
        stream_.fill(0, total);
        expected_ = total;
        filled_ = 0;
        holed_ = (seqOffset != 0);
        resol_ = resol;
        absOffset_ = absOffset;
        samples_ = samples;
        channels_ = channels;
    }

    const int part = proto.readAvailable();
    if (part < 0 || seqOffset + part > expected_) {
        holed_ = true;
        return respErrorPayload;
    }

    if (seqOffset != filled_) {
        holed_ = true;
    }
    proto.read(reinterpret_cast<uint8_t*>(stream_.data()) + seqOffset, part);
    filled_ = seqOffset + part;

    if (filled_ < expected_) {
        return respOk; // still arriving
    }

    const int coded = expected_;
    expected_ = 0; // whatever happens next, the next slice starts a new ping

    if (holed_) {
        ++dropped_;
        return respErrorPayload;
    }

    decoded_.resize(samples_);
    const int got = kec_decode_ping(reinterpret_cast<const uint8_t*>(stream_.constData()),
                                    coded,
                                    reinterpret_cast<uint8_t*>(decoded_.data()),
                                    static_cast<int>(decoded_.size()));
    if (got <= 0) {
        ++undecoded_;
        return respErrorPayload;
    }

    if (sink_ != nullptr) {
        sink_->acceptDecoded(reinterpret_cast<const uint8_t*>(decoded_.constData()),
                             got, resol_, absOffset_, channels_);
    }

    return respOk;
}

Resp IDBinChart::parsePayload(FrameParser &proto) {
    if (proto.ver() == v3) {   // echocodec range blocks, as SonarN sends them (ChartV3Assembler, docs/ECHO_CODECS.md)
        const int len = proto.readAvailable();
        if (len <= 0) {
            return respErrorPayload;
        }
        std::vector<uint8_t> frame(static_cast<size_t>(len));
        proto.read(frame.data(), len);
        std::vector<ChartV3Assembler::Ping> pings;
        const bool ok = v3_.feed(frame.data(), len, pings);
        // Two pings come out of one frame only when a block of the next ping ends one that had blocks missing and
        // is itself a whole ping; the second replaces the first before anyone reads it.
        for (const ChartV3Assembler::Ping& p : pings) {
            ++v3Pings_;
            v3Filled_ += static_cast<uint32_t>(p.missingBlocks);
            lossIndex_ = (lossIndex_ + 1) % lossHistory_.size();
            lossHistory_[lossIndex_] = p.missingBlocks ? 1 : 0;
            acceptDecoded(p.cells.data(), static_cast<int>(p.cells.size()), p.resolMm, 0, 1);
        }
        return ok ? respOk : respErrorPayload;
    }
    if(proto.ver() == v0 || proto.ver() == v1) {
        U2 m_seqOffset = proto.read<U2>();
        U2 sampleResol = proto.read<U2>();
        U2 absOffset = proto.read<U2>();

        // A new resolution or offset ends the chart being filled -- compared with that chart's own
        // (sampleResolLast_), and only when there is one. Compared with the last completed chart's,
        // the first part after connecting and the second part after every change "completed" a
        // chart too: an empty one, then the first part alone, which went into an epoch of its own
        // and left every later chart one epoch off its heading (2026-10-17).
        const bool newGeometry = m_chartSizeIncr != 0 && (sampleResolLast_ != sampleResol || absOffsetLast_ != absOffset);
        if ((m_seqOffset == 0 && m_chartSizeIncr != 0) || newGeometry) {
            if(proto.ver() == v0) {
                memcpy(m_completeChart, m_fillChart, m_chartSizeIncr);
                m_chartSize = m_chartSizeIncr;
                compErrList_ = std::move(tempErrList_);
                tempErrList_.clear();
            }
                else if(proto.ver() == v1) {
                    for (uint32_t i = 0; i < m_chartSizeIncr / 2; i++) {
                        const size_t baseIndex = static_cast<size_t>(i) * size_t{2};
                        m_completeChart[i] = m_fillChart[baseIndex];
                        m_completeChart2[i] = m_fillChart[baseIndex + 1];
                    }
                    m_chartSize = m_chartSizeIncr/2;
                    compErrList_ = std::move(tempErrList_);
                tempErrList_.clear();
            }

            m_sampleResol = sampleResolLast_;
            m_absOffset = absOffsetLast_;

            m_isCompleteChart = true;

            m_chartSizeIncr = 0;
        }

        // if(m_seqOffset == 0 || m_sampleResol != sampleResol || m_absOffset != absOffset) {
        //     // m_sampleResol = sampleResol;
        //     // m_absOffset = absOffset;
        //     m_chartSizeIncr = 0;
        // }

        lossIndex_ = (lossIndex_ + 1) % lossHistory_.size();

        uint16_t part_len = proto.readAvailable();
        if (m_chartSizeIncr == m_seqOffset) {
            lossHistory_[lossIndex_] = 0;

            if(m_seqOffset + part_len < sizeof (m_fillChart)) {
                proto.read(&m_fillChart[m_chartSizeIncr], part_len);
                m_chartSizeIncr += part_len;
            }
        }
        else if(m_chartSizeIncr < m_seqOffset) {
            lossHistory_[lossIndex_] = 1;


            if (proto.ver() == v0) {
                tempErrList_.append(qMakePair(m_chartSizeIncr, m_seqOffset));
            }
            else if (proto.ver() == v1) {
                tempErrList_.append(qMakePair(m_chartSizeIncr / 2, m_seqOffset / 2));
            }

            for (int i = m_chartSizeIncr; i < m_seqOffset; i++) {
                m_fillChart[i] = 0;
            }
            if (m_seqOffset + part_len < sizeof(m_fillChart)) {
                proto.read(&m_fillChart[m_seqOffset], part_len);
                m_chartSizeIncr = m_seqOffset + part_len;
            }
            //return respErrorPayload;
        }
        else { // first frame loss
            if ((m_chartSizeIncr != 0) || (m_sampleResol != sampleResol || m_absOffset != absOffset)) {
                if(proto.ver() == v0) {
                    memcpy(m_completeChart, m_fillChart, m_chartSizeIncr);
                    m_chartSize = m_chartSizeIncr;
                    compErrList_ = std::move(tempErrList_);
                    tempErrList_.clear();
                }
                else if(proto.ver() == v1) {
                    for (uint32_t i = 0; i < m_chartSizeIncr / 2; i++) {
                        const size_t baseIndex = static_cast<size_t>(i) * size_t{2};
                        m_completeChart[i] = m_fillChart[baseIndex];
                        m_completeChart2[i] = m_fillChart[baseIndex + 1];
                    }
                    compErrList_ = std::move(tempErrList_);
                    tempErrList_.clear();
                    m_chartSize = m_chartSizeIncr / 2;
                }

                m_sampleResol = sampleResolLast_;
                m_absOffset = absOffsetLast_;

                m_isCompleteChart = true;
            }

            m_chartSizeIncr = 0;

            if (proto.ver() == v0) {
                tempErrList_.append(qMakePair(m_chartSizeIncr, m_seqOffset));
            }
            else if (proto.ver() == v1) {
                tempErrList_.append(qMakePair(m_chartSizeIncr / 2, m_seqOffset / 2));
            }

            for (int i = m_chartSizeIncr; i < m_seqOffset; i++) {
                m_fillChart[i] = 0;
            }

            if (m_seqOffset + part_len < sizeof(m_fillChart)) {
                proto.read(&m_fillChart[m_seqOffset], part_len);
                m_chartSizeIncr = m_seqOffset + part_len;
            }
        }

        sampleResolLast_ = sampleResol;
        absOffsetLast_ = absOffset;
    }
    else if(proto.ver() == v7) {
        RawData::RawDataHeader header;

        proto.read(&header);

        int avail = proto.readAvailable();
        QByteArray data = QByteArray((char*)proto.read(avail), avail);
        int expected_data_offset = header.localOffset*(header.dataSize+1)*header.channelCount;

        int ch_group = header.channelGroup;

        if(header.localOffset == 0) {
            if(_rawData[ch_group].data.size() != 0) {
                _rawDataComplete[ch_group] = std::move(_rawData[ch_group]);
                emit rawDataRecieved(_rawDataComplete[ch_group]);
            }

            _rawData[ch_group] = {header, data};
        } else  if(_rawData.contains(ch_group)){
            RawData& raw = _rawData[ch_group];
            int ready_samples = raw.samplesPerChannel();

            if (static_cast<uint32_t>(ready_samples) == header.localOffset) {
                if(expected_data_offset == raw.data.size()) {
                    raw.data.append(data);
                } else {
                    qDebug() << "Raw data error: data offset";
                }
            } else {
                raw.data.insert(expected_data_offset, data);
               qDebug() << "Raw data error: sample counter";
            }
        }
    }
    else {
        return respErrorVersion;
    }

    return respOk;
}

Resp IDBinAttitude::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
        const float scale_to_deg = 0.01f;
        m_yaw = static_cast<float>(proto.read<S2>())*scale_to_deg;
        m_pitch = static_cast<float>(proto.read<S2>())*scale_to_deg;
        m_roll = static_cast<float>(proto.read<S2>())*scale_to_deg;
    } else if(proto.ver() == v1) {
       /* All four used to land in m_w0, each overwriting the last, leaving the fourth component
          in the first slot and zeros in the other three -- not an orientation at all. Nothing
          sends v1 today and nothing calls w1()..w3(), so it never showed; it would have shown up
          as a silently wrong attitude the first time a device did send a quaternion. */
       m_w0 = proto.read<F4>();
       m_w1 = proto.read<F4>();
       m_w2 = proto.read<F4>();
       m_w3 = proto.read<F4>();
    } else {
        return respErrorVersion;
    }

    return respOk;
}


float IDBinAttitude::yaw(Version src_ver) {
    Q_UNUSED(src_ver);
    return m_yaw;
}

float IDBinAttitude::pitch(Version src_ver) {
    Q_UNUSED(src_ver);
    return m_pitch;
}

float IDBinAttitude::roll(Version src_ver) {
    Q_UNUSED(src_ver);
    return m_roll;
}

float IDBinAttitude::w0(Version src_ver) {
    Q_UNUSED(src_ver);
    return m_w0;
}
float IDBinAttitude::w1(Version src_ver) {
    Q_UNUSED(src_ver);
    return m_w1;
}
float IDBinAttitude::w2(Version src_ver) {
    Q_UNUSED(src_ver);
    return m_w2;
}
float IDBinAttitude::w3(Version src_ver) {
    Q_UNUSED(src_ver);
    return m_w3;
}

Resp IDBinTemp::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
        const float scale_to_cels = 0.01f;
        m_temp = static_cast<float>(proto.read<S2>())*scale_to_cels;
    } else {
        return respErrorVersion;
    }

    return respOk;
}

Resp IDBinEncoder::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
        Encoder tmp;
        if(proto.readAvailable() >= 4) {
            tmp.e1 = proto.read<F4>();
        }
        if(proto.readAvailable() >= 4) {
            tmp.e2 = proto.read<F4>();
        }
        if(proto.readAvailable() >= 4) {
            tmp.e3 = proto.read<F4>();
        }
        data = tmp;
    } else {
        return respErrorVersion;
    }

    return respOk;
}

Resp IDBinNav::parsePayload(FrameParser &proto) {
    if(proto.ver() == SimpleNav::getVer()) {
        if (proto.readAvailable() < static_cast<int16_t>(sizeof(SimpleNav))) {
            return respErrorPayload;
        }
        SimpleNav nav = proto.read<SimpleNav>();
        _nav = nav;
    } else if (proto.ver() == SimpleNavV2::getVer()) {
        if (proto.readAvailable() < static_cast<int16_t>(sizeof(SimpleNavV2))) {
            return respErrorPayload;
        }
        SimpleNavV2 nav = proto.read<SimpleNavV2>();
        _navV2 = nav;
    } else {
        return respErrorVersion;
    }

    return respOk;
}

Resp IDBinBoatStatus::parsePayload(FrameParser& proto)
{
    if (proto.ver() != BoatStatus::getVer()) {
        return respErrorVersion;
    }

    if (proto.readAvailable() < static_cast<int16_t>(sizeof(BoatStatus))) {
        return respErrorPayload;
    }

    data_ = proto.read<BoatStatus>();
    return respOk;
}

static_assert(sizeof(IDBinRecorderStatus::RecorderStatus) == 21,
              "RecorderStatusV0 wire layout must stay 21 bytes");

Resp IDBinRecorderStatus::parsePayload(FrameParser& proto)
{
    if (proto.ver() != RecorderStatus::getVer()) {
        return respErrorVersion;
    }

    if (proto.readAvailable() < static_cast<int16_t>(sizeof(RecorderStatus))) {
        return respErrorPayload;
    }

    data_ = proto.read<RecorderStatus>();
    valid_ = true;
    return respOk;
}

static_assert(sizeof(IDBinNavSensorStatus::NavSensorStatus) == 38,
              "NavSensorStatusV0 wire layout must stay 38 bytes");

Resp IDBinNavSensorStatus::parsePayload(FrameParser& proto)
{
    if (proto.ver() != NavSensorStatus::getVer()) {
        return respErrorVersion;
    }

    if (proto.readAvailable() < static_cast<int16_t>(sizeof(NavSensorStatus))) {
        return respErrorPayload;
    }

    data_ = proto.read<NavSensorStatus>();
    valid_ = true;
    return respOk;
}

Resp IDBinDVL::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
         vel_x = proto.read<F4>();
         vel_y = proto.read<F4>();
         vel_z = proto.read<F4>();
         _dist = proto.read<F4>();

//         qInfo("DVL x: %f, y: %f, z: %f, d: %f", vel_x, vel_y, vel_z, _dist);
    } else  if(proto.ver() == v1) {
        _beamCount = 0;
        for(uint8_t i = 0; i < 4 && static_cast<int16_t>(sizeof(BeamSolution)) <= proto.readAvailable(); i++) {
            _beams[i] = proto.read<BeamSolution>();
            _beamCount++;
        }

        if(_beams[0].mode > 0 && _beams[0].mode <= 3) {
            test_bias += _beams[0].velocity*_beams[0].dt;
        }
    } else if(proto.ver() == v2) {
        if (static_cast<int16_t>(sizeof(DVLSolution)) <= proto.readAvailable()) {
            _dvlSolution = proto.read<DVLSolution>();
//            qInfo("DVL x: %f, y: %f, z: %f, d: %f", _dvlSolution.velocity.x, _dvlSolution.velocity.y, _dvlSolution.velocity.z, _dvlSolution.distance.z);
        } else {
            return respErrorPayload;
        }
    } else {
        return respErrorVersion;
    }

    return respOk;
}


Resp IDBinDataset::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0) {
        const uint8_t chId = proto.read<U1>();
        if (chId < _countof(m_channel)) {
            m_channel[chId].id = chId;
            m_channel[chId].period = proto.read<U4>();
            m_channel[chId].mask = proto.read<U4>();
        }
    }
    else
        return respErrorVersion;

    return respOk;
}

void IDBinDataset::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinDataset::setChannel(uint8_t ch_id, uint32_t period, uint32_t mask) {
    sendChannel(ch_id, period, mask);
}

uint32_t IDBinDataset::mask(U1 ch_id) {
    uint32_t mask = 0;
    if(ch_id == 0) {
        for(int i = 1; i < 3; i++) {
            mask |= m_channel[i].mask;
        }
    } else if(ch_id < _countof(m_channel)) {
        mask = m_channel[ch_id].mask;
    }
    return mask;
}

void IDBinDataset::setMask(U1 ch_id, uint32_t mask) {
    m_channel[ch_id].id = ch_id;
    m_channel[ch_id].mask = mask;
}

uint32_t IDBinDataset::period(U1 ch_id) {
    if(ch_id < _countof(m_channel)) {
        return m_channel[ch_id].period;
    }
    return 0;
}

void IDBinDataset::setPeriod(U1 ch_id, uint32_t period) {
    sendChannel(ch_id, period, mask(ch_id));
}

QVector<IDBinDataset::Channel> IDBinDataset::getChannels() const {
    const uint16_t channelsSize{3};
    QVector<IDBinDataset::Channel> retVal;
    retVal.reserve(channelsSize);
    for (uint16_t i = 0; i < channelsSize; ++i)
        retVal.push_back(m_channel[i]);
    return retVal;
}

IDBinDataset::Channel IDBinDataset::getChannel(U1 channelId) const
{
    if (channelId < 3)
        return m_channel[channelId];

    return {};
}

void IDBinDataset::sendChannel(U1 ch_id, uint32_t period, uint32_t mask) {
    if(ch_id < _countof(m_channel)) {
        m_channel[ch_id].period = period;

        ProtoBinOut id_out;
        id_out.create(SETTING, v0, id(), m_address);
        id_out.write<U1>(ch_id);
        id_out.write<U4>(period);
        id_out.write<U4>(mask);
        id_out.end();

        hashBinFrameOut(id_out);
    }
}


Resp IDBinDistSetup::parsePayload(FrameParser &proto) {
    if (proto.ver() == v1) {
        m_startOffset = proto.read<U4>();
        m_maxDist = proto.read<U4>();
    }
    else if (proto.ver() == v2)
        m_confidence = proto.read<U1>();
    else
        return respErrorVersion;

    return respOk;
}

void IDBinDistSetup::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinDistSetup::setRange(uint32_t start_offset, uint32_t max_dist) {
    m_startOffset = start_offset;
    m_maxDist = max_dist;

    ProtoBinOut id_out;
    id_out.create(SETTING, v1, id(), m_address);
    id_out.write<U4>(start_offset);
    id_out.write<U4>(max_dist);
    id_out.end();

    hashBinFrameOut(id_out);
}

void IDBinDistSetup::setConfidence(int confidence) {
    m_confidence = confidence;

    ProtoBinOut id_out;
    id_out.create(SETTING, v2, id(), m_address);
    id_out.write<U1>(m_confidence);
    id_out.end();

    hashBinFrameOut(id_out);
}


namespace {
// [length u8][that many bytes], cut to what the frame has; false when not even the length is there.
bool readLabel(FrameParser& proto, QString* out)
{
    if (proto.readAvailable() < 1) {
        return false;
    }
    const int len = qMin<int>(proto.read<U1>(), proto.readAvailable());
    QByteArray b(len, '\0');
    if (len > 0) {
        proto.read(reinterpret_cast<uint8_t*>(b.data()), int16_t(len));
    }
    *out = QString::fromLatin1(b);
    return true;
}
}

Resp IDBinBus::parsePayload(FrameParser &proto) {
    const Version ver = proto.ver();
    if (ver == v2 || ver == v3 || ver == v4) {   // a UART function's settings
        const int need = ver == v2 ? 8 : ver == v3 ? 6 : 7;
        if (proto.readAvailable() < need) {
            return respErrorPayload;
        }
        const U1 function = proto.read<U1>();
        if (functionVersion(function) != ver) {
            return respErrorPayload;
        }
        FunctionSettings f = m_func[function];
        f.baud = proto.read<U4>();
        if (ver == v2) {
            f.protocol = proto.read<U1>();
            f.log = proto.read<U1>();
            f.toHost = proto.read<U1>();
        } else if (ver == v3) {
            f.toHost = proto.read<U1>();
        } else {
            f.protocol = proto.read<U1>();
            f.rateHz = proto.read<U1>();
        }
        f.valid = true;
        m_func[function] = f;
        return respOk;
    }
    if (proto.ver() != v0) {
        return respErrorVersion;
    }
    if (proto.readAvailable() < 16) {
        return respErrorPayload;
    }
    Bus b;
    b.id = proto.read<U1>();
    const U1 count = proto.read<U1>();
    b.supply = proto.read<U1>();
    b.functionCaps = proto.read<U1>();
    b.function = proto.read<U1>();
    b.functionSaved = proto.read<U1>();
    b.uartCaps = proto.read<U1>();
    b.uart = proto.read<U1>();
    b.uartSaved = proto.read<U1>();
    b.txPin = proto.read<U1>();
    b.rxPin = proto.read<U1>();
    b.baud = proto.read<U4>();
    const int pinCount = proto.read<U1>();
    if (proto.readAvailable() < pinCount) {
        return respErrorPayload;
    }
    for (int i = 0; i < pinCount; ++i) {
        b.pins.append(proto.read<U1>());
    }
    if (!readLabel(proto, &b.label)) {
        return respErrorPayload;
    }
    m_count = count;
    m_last = b.id;
    m_buses[b.id] = b;
    return respOk;
}

void IDBinBus::request(U1 bus) {
    ProtoBinOut id_out;
    id_out.create(GETTING, v0, id(), m_address);
    id_out.write<U1>(bus);
    id_out.end();
    emit binFrameOut(id_out);
}

Version IDBinBus::functionVersion(int function) {
    return function == 1 || function == 2 ? v2 : function == FunctionTelemetry ? v3 : function == 4 ? v4 : v0;
}

const IDBinBus::FunctionSettings& IDBinBus::functionSettings(int function) const {
    return m_func[function >= 1 && function <= 4 ? function : 0];
}

void IDBinBus::requestFunction(U1 function) {
    ProtoBinOut id_out;
    id_out.create(GETTING, functionVersion(function), id(), m_address);
    id_out.write<U1>(function);
    id_out.end();
    emit binFrameOut(id_out);
}

void IDBinBus::setFunctionBaud(U1 function, U4 baud) {
    const Version ver = functionVersion(function);
    const FunctionSettings& f = functionSettings(function);
    ProtoBinOut id_out;
    id_out.create(SETTING, ver, id(), m_address);
    id_out.write<U1>(function);
    id_out.write<U4>(baud);
    if (ver == v2) {
        id_out.write<U1>(f.protocol);
        id_out.write<U1>(f.log);
        id_out.write<U1>(f.toHost);
    } else if (ver == v3) {
        id_out.write<U1>(f.valid ? f.toHost : U1(0));   // telemetry to the host: the firmware takes 0 only
    } else {
        id_out.write<U1>(f.protocol);
        id_out.write<U1>(f.rateHz);
    }
    id_out.end();
    emit binFrameOut(id_out);
}

void IDBinBus::setBusFunction(U1 bus, U1 function, U1 uart) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U1>(bus);
    id_out.write<U1>(function);
    id_out.write<U1>(uart);
    id_out.end();
    emit binFrameOut(id_out);
}

const QVector<U4>& IDBinBus::functionBauds() {
    static const QVector<U4> bauds = { 0, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 2000000, 5000000 };
    return bauds;
}

Resp IDBinPin::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0) {
        if (proto.readAvailable() < 14) {
            return respErrorPayload;
        }
        Pin p;
        p.id = proto.read<U1>();
        p.bus = proto.read<U1>();
        p.roleCaps = proto.read<U4>();
        p.role = proto.read<U1>();
        p.roleSaved = proto.read<U1>();
        p.timer = proto.read<U1>();
        p.param = proto.read<U1>();
        p.value = proto.read<S4>();
        if (!readLabel(proto, &p.label) || !readLabel(proto, &p.mcuPin)) {
            return respErrorPayload;
        }
        m_pins[p.id] = p;
        m_last = p.id;
        return respOk;
    }
    if (proto.ver() == v1) {
        if (proto.readAvailable() < 1) {
            return respErrorPayload;
        }
        const int count = proto.read<U1>();
        if (proto.readAvailable() < count * 6) {
            return respErrorPayload;
        }
        for (int i = 0; i < count; ++i) {
            const U1 id = proto.read<U1>();
            const U1 role = proto.read<U1>();
            const S4 value = proto.read<S4>();
            auto it = m_pins.find(id);
            if (it != m_pins.end()) {   // a pin not described yet waits for its v0
                it->role = role;
                it->value = value;
            }
        }
        m_last = -2;
        return respOk;
    }
    return respErrorVersion;
}

void IDBinPin::request(U1 pin) {
    ProtoBinOut id_out;
    id_out.create(GETTING, v0, id(), m_address);
    id_out.write<U1>(pin);
    id_out.end();
    emit binFrameOut(id_out);
}

void IDBinPin::requestValues() {
    simpleRequest(v1);
}

Resp IDBinRingStat::parsePayload(FrameParser &proto) {
    if (proto.ver() != v0) {
        return respErrorVersion;
    }
    if (proto.readAvailable() < kFrameBytes) {
        return respErrorPayload;
    }
    State s;
    s.address = proto.read<U1>();
    s.role = proto.read<U1>();
    s.upstream = proto.read<U1>();
    proto.read<U1>();   // spare
    s.guardUs = proto.read<U4>();
    s.breakIntervalUs = proto.read<U4>();
    s.relayed = proto.read<U4>();
    s.breaks = proto.read<U4>();
    s.torn = proto.read<U4>();
    s.timeouts = proto.read<U4>();
    s.skipped = proto.read<U4>();
    s.known = true;
    m_state = s;
    return respOk;
}

void IDBinRingStat::reset() {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    appendKey(id_out);
    id_out.end();
    emit binFrameOut(id_out);   // not tracked: the state frame after RESP 1 is the confirmation
}

Resp IDBinServoControl::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0) {
        m_general       = proto.read<U2>();
        m_options       = proto.read<U2>();
        m_pwmMinUs      = proto.read<U2>();
        m_pwmMaxUs      = proto.read<U2>();
        m_angleRangeDeg = proto.read<S2>();
        m_stepDeg       = proto.read<S2>();
        m_rangeDeg      = proto.read<S2>();
        m_centerDeg     = proto.read<S2>();
        if (manual()) {
            m_manualDeg = m_centerDeg;
        }
    } else if (proto.ver() == v1) {
        m_pwmHz = proto.read<U2>();
    } else if (proto.ver() == v2) {
        m_trigKind     = proto.read<U1>();
        m_trigFunction = proto.read<U1>();
        m_trigAddress  = proto.read<U1>();
    } else {
        return respErrorVersion;
    }
    return respOk;
}

void IDBinServoControl::setPwmHz(U2 hz) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v1, id(), m_address);
    id_out.write<U2>(hz);
    id_out.end();
    emit binFrameOut(id_out);

    simpleRequest(v1);
}

void IDBinServoControl::setTrigger(U1 function, U1 address) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v2, id(), m_address);
    id_out.write<U1>(TrigSonarPing);
    id_out.write<U1>(function);
    id_out.write<U1>(address);
    id_out.end();
    emit binFrameOut(id_out);

    simpleRequest(v2);
}

void IDBinServoControl::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinServoControl::setAll(U2 general, U2 options, U2 pwm_min_us, U2 pwm_max_us,
                               S2 angle_range_deg, S2 step_deg, S2 range_deg, S2 center_deg) {
    m_general       = general;
    m_options       = options;
    m_pwmMinUs      = pwm_min_us;
    m_pwmMaxUs      = pwm_max_us;
    m_angleRangeDeg = angle_range_deg;
    m_stepDeg       = step_deg;
    m_rangeDeg      = range_deg;
    m_centerDeg     = center_deg;

    send(true);
}

void IDBinServoControl::setManual(bool on) {
    if (on == manual()) {
        return;
    }
    if (on) {
        m_scanCenterDeg = m_centerDeg;
        m_manualDeg = clampManual(m_centerDeg);
        m_general |= GenManual;
        m_centerDeg = m_manualDeg;
    } else {
        m_general &= ~GenManual;
        m_centerDeg = m_scanCenterDeg;
    }
    send(false);
}

void IDBinServoControl::setManualDeg(S2 deg) {
    if (!manual()) {
        return;
    }
    m_manualDeg = clampManual(deg);
    m_centerDeg = m_manualDeg;
    send(false);
}

void IDBinServoControl::dropManual() {
    if (!manual()) {
        return;
    }
    m_general &= ~GenManual;
    m_centerDeg = m_scanCenterDeg;
}

S2 IDBinServoControl::clampManual(S2 deg) const {
    const int half = qMax(0, int(m_angleRangeDeg)) / 2;
    return static_cast<S2>(qBound(-half, int(deg), half));
}

void IDBinServoControl::send(bool tracked) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U2>(m_general);
    id_out.write<U2>(m_options);
    id_out.write<U2>(m_pwmMinUs);
    id_out.write<U2>(m_pwmMaxUs);
    id_out.write<S2>(m_angleRangeDeg);
    id_out.write<S2>(m_stepDeg);
    id_out.write<S2>(m_rangeDeg);
    id_out.write<S2>(m_centerDeg);
    id_out.end();

    if (tracked) {
        hashBinFrameOut(id_out);
    } else {
        emit binFrameOut(id_out);
    }
}


Resp IDBinPwmRoute::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0) {
        for (int i = 0; i < PwmOutCount; ++i) {
            m_target[i] = proto.read<U1>();
        }
    } else {
        return respErrorVersion;
    }
    return respOk;
}

void IDBinPwmRoute::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinPwmRoute::setRoute(U1 out1, U1 out2, U1 out3) {
    m_target[0] = out1;
    m_target[1] = out2;
    m_target[2] = out3;

    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U1>(out1);
    id_out.write<U1>(out2);
    id_out.write<U1>(out3);
    id_out.end();

    hashBinFrameOut(id_out);
}


Resp IDBinStandScan::parsePayload(FrameParser &proto) {
    Q_UNUSED(proto)
    // Nothing to read: the stand publishes no state. Everything this class learns arrives as
    // the response code to a command it sent, which IDBin::parse handles before this point.
    return respOk;
}

void IDBinStandScan::command(Command cmd) {
    Scan idle;
    idle.innerStep = 1;
    idle.outerStep = 1;
    writeScan(cmd, idle);
}

void IDBinStandScan::probe() {
    command(CmdNop);
}

void IDBinStandScan::start(const Scan &scan) {
    writeScan(CmdStart, scan);
}

void IDBinStandScan::writeScan(Command cmd, const Scan &scan) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U1>(static_cast<U1>(cmd));
    id_out.write<U1>(scan.order);
    id_out.write<U1>(scan.options);
    id_out.write<U2>(scan.fires);
    id_out.write<U2>(scan.cycles);
    id_out.write<U2>(scan.settleMs);
    id_out.write<U2>(scan.postFireMs);
    id_out.write<S4>(scan.innerStart);
    id_out.write<S4>(scan.innerEnd);
    id_out.write<S4>(scan.innerStep);
    id_out.write<S4>(scan.outerStart);
    id_out.write<S4>(scan.outerEnd);
    id_out.write<S4>(scan.outerStep);
    id_out.end();

    emit binFrameOut(id_out);
}


Resp IDBinDevSync::parsePayload(FrameParser &proto) {
    if (proto.ver() != v0)         return respErrorVersion;
    if (proto.readAvailable() < 2) return respErrorPayload;

    m_periodMs = proto.read<U2>();

    const int n = proto.readAvailable();
    m_portSource.resize(n);
    for (int i = 0; i < n; ++i)
        m_portSource[i] = static_cast<char>(proto.read<U1>());

    m_synced = true;
    return respOk;
}

void IDBinDevSync::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinDevSync::flushPending() {
    if (!m_synced || !m_pending) return;

    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U2>(m_periodMs);
    for (char b : std::as_const(m_portSource))
        id_out.write<U1>(static_cast<U1>(b));
    id_out.end();

    hashBinFrameOut(id_out);
    m_pending = false;
}


Resp IDBinChartSetup::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0) {
        m_sanpleCount = proto.read<U2>();
        m_sanpleResolution = proto.read<U2>();
        m_sanpleOffset = proto.read<U2>();
    }
    else
        return respErrorVersion;

    return respOk;
}

void IDBinChartSetup::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinChartSetup::setV0(uint16_t count, uint16_t resolution, uint16_t offset) {
    m_sanpleCount = count;
    if(count*resolution > 200000) {
        resolution = (200000/count/10)*10;
    }
    m_sanpleResolution = resolution;
    m_sanpleOffset = offset;

    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U2>(count);
    id_out.write<U2>(resolution);
    id_out.write<U2>(offset);
    id_out.end();

    hashBinFrameOut(id_out);
}

Resp IDBinDSPSetup::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0) {
        m_horSmoothFactor = proto.read<U1>();
        //qInfo("read smooth %u", m_horSmoothFactor);
    }
    else
        return respErrorVersion;

    return respOk;
}

void IDBinDSPSetup::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinDSPSetup::setV0(U1 hor_smooth_factor) {
    m_horSmoothFactor = hor_smooth_factor;

    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U1>(horSmoothFactor());
    id_out.end();
    qInfo("write smooth %u", m_horSmoothFactor);

    hashBinFrameOut(id_out);
}


Resp IDBinTransc::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0) {
        m_freq = proto.read<U2>();
        m_pulse = proto.read<U1>();
        m_boost = proto.read<U1>();
    }
    else
        return respErrorVersion;

    return respOk;
}

void IDBinTransc::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinTransc::setTransc(U2 freq, U1 pulse, U1 boost) {
    m_freq = freq;
    m_pulse = pulse;
    m_boost = boost;

    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U2>(freq);
    id_out.write<U1>(pulse);
    id_out.write<U1>(boost);
    id_out.end();

    hashBinFrameOut(id_out);
}


bool IDBinNet::addrValue(const QString& s, quint32* out) {
    const QStringList parts = s.trimmed().split('.');
    if (parts.size() != 4) return false;
    quint32 v = 0;
    for (const QString& part : parts) {
        bool ok = false;
        const int o = part.toInt(&ok);
        if (!ok || o < 0 || o > 255 || part.isEmpty()) return false;
        v = (v << 8) | quint32(o);
    }
    *out = v;
    return true;
}

Resp IDBinNet::parsePayload(FrameParser &proto) {
    if (proto.ver() != v0) return respErrorVersion;
    if (proto.readAvailable() < 20) return respErrorPayload;

    const uint8_t ver = proto.read<U1>();
    if (ver != 1) return respErrorVersion;      // a layout we do not know
    m_flags = proto.read<U1>();
    QString mac;
    for (int i = 0; i < 6; ++i) {
        mac += QStringLiteral("%1").arg(proto.read<U1>(), 2, 16, QLatin1Char('0')).toUpper();
        if (i < 5) mac += ':';
    }
    m_mac = mac;
    quint32 a[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 4; ++i) a[k] = (a[k] << 8) | quint32(proto.read<U1>());
    m_ip = a[0];
    m_mask = a[1];
    m_host = a[2];
    m_valid = true;
    return respOk;
}

bool IDBinNet::setNet(const QString& ip, const QString& mask, const QString& host, bool dhcp) {
    quint32 a = 0, m = 0, h = 0;
    if (!addrValue(ip, &a) || !addrValue(mask, &m) || !addrValue(host, &h)) return false;

    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U1>(1);                        // layout version
    id_out.write<U1>(dhcp ? FlagDhcp : 0);
    for (int i = 0; i < 6; ++i) id_out.write<U1>(0xFF);   // any device on the link
    const quint32 v[3] = { a, m, h };
    for (int k = 0; k < 3; ++k)
        for (int i = 3; i >= 0; --i) id_out.write<U1>(uint8_t((v[k] >> (8 * i)) & 0xFF));
    id_out.end();

    hashBinFrameOut(id_out);
    return true;
}

Resp IDBinSoundSpeed::parsePayload(FrameParser &proto)
{
    if (proto.ver() == v0)
        m_soundSpeed = proto.read<U4>();
    else
        return respErrorVersion;

    return respOk;
}

void IDBinSoundSpeed::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinSoundSpeed::setSoundSpeed(U4 snd_spd) {
    m_soundSpeed = snd_spd;

    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U4>(snd_spd);
    id_out.end();

    hashBinFrameOut(id_out);
}

Resp IDBinUART::parsePayload(FrameParser &proto)
{
    if(proto.ver() == v0) {
        if(checkKeyConfirm(proto.read<U4>())) {
            uint8_t uart_id = proto.read<U1>();

            if(uart_id < _countof(m_uart)) {
                m_uart[uart_id].id = uart_id;
                m_uart[uart_id].baudrate = proto.read<U4>();
            }
        } else {
            return respErrorKey;
        }
    } else if(proto.ver() == v1) {
        if(checkKeyConfirm(proto.read<U4>())) {
            uint8_t uart_id = proto.read<U1>();

            if(uart_id < _countof(m_uart)) {
                m_uart[uart_id].id = uart_id;
                m_uart[uart_id].dev_address = proto.read<U1>();
            }
        } else {
            return respErrorKey;
        }
    } else if(proto.ver() == v2) {
        if(checkKeyConfirm(proto.read<U4>())) {
            devDef_address = proto.read<U1>();
        } else {
            return respErrorKey;
        }
    }
    else
        return respErrorVersion;

    return respOk;
}

void IDBinUART::startColdStartTimer()
{
    interExecColdStartTimer();
}

void IDBinUART::setBaudrate(U4 baudrate) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    appendKey(id_out);

    id_out.write<U1>(1);
    qInfo("setBaudrate %u", baudrate);
    m_uart[1].baudrate = baudrate;
    id_out.write<U4>(m_uart[1].baudrate);

    id_out.end();

    hashBinFrameOut(id_out);
}

void IDBinUART::setDevAddress(U1 addr) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v1, id(), m_address);
    appendKey(id_out);

    id_out.write<U1>(1);
    m_uart[1].dev_address = addr;
    id_out.write<U1>(addr);

    id_out.end();

    hashBinFrameOut(id_out);
}

void IDBinUART::setDevDefAddress(U1 addr) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v2, id(), m_address);
    appendKey(id_out);

    devDef_address = addr;
    id_out.write<U1>(addr);

    id_out.end();

    hashBinFrameOut(id_out);
}

QVector<IDBinUART::UART> IDBinUART::getUart() const {
    const uint16_t uartSize {5};
    QVector<UART> retVal;
    retVal.reserve(uartSize);
    for (uint16_t i = 0; i < uartSize; ++i)
        retVal.push_back(m_uart[i]);
    return retVal;
}

QString IDBinWifi::readAddr(FrameParser& proto) {
    const U1 a = proto.read<U1>();
    const U1 b = proto.read<U1>();
    const U1 c = proto.read<U1>();
    const U1 d = proto.read<U1>();
    return QString("%1.%2.%3.%4").arg(a).arg(b).arg(c).arg(d);
}

QString IDBinWifi::readMac(FrameParser& proto) {
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    proto.read(mac, 6);
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(mac), 6).toHex(':'));
}

QString IDBinWifi::readSsid(FrameParser& proto) {
    int len = proto.read<U1>();
    if (len > proto.readAvailable()) {
        len = proto.readAvailable();
    }
    if (len <= 0) {
        return QString();
    }
    QByteArray raw(len, 0);
    proto.read(reinterpret_cast<uint8_t*>(raw.data()), static_cast<int16_t>(len));
    return QString::fromUtf8(raw);
}

Resp IDBinWifi::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0) {
        if (proto.readAvailable() < 39) {
            return respErrorPayload;
        }
        Status s;
        s.valid = true;
        s.state = proto.read<U1>();
        s.flags = proto.read<U1>();
        s.rssi = proto.read<S1>();
        s.channel = proto.read<U1>();
        s.security = proto.read<U1>();
        s.disconnectCode = proto.read<U1>();
        s.reason = proto.read<U1>();
        s.clients = proto.read<U1>();           // AP clients (0.10); reserved before
        s.bssid = readMac(proto);
        s.ip = readAddr(proto);
        s.mask = readAddr(proto);
        s.gateway = readAddr(proto);
        s.dns = readAddr(proto);
        s.rxBps = proto.read<U4>();
        s.txBps = proto.read<U4>();
        s.ssid = readSsid(proto);
        status_ = s;
        return respOk;
    }
    if (proto.ver() == v1) {
        if (proto.readAvailable() < 22) {
            return respErrorPayload;
        }
        report_.valid = true;
        report_.state = proto.read<U1>();
        report_.rssi = proto.read<S1>();
        report_.rxBps = proto.read<U4>();
        report_.txBps = proto.read<U4>();
        report_.rxTotal = proto.read<U4>();
        report_.txTotal = proto.read<U4>();
        report_.uptimeS = proto.read<U4>();
        // 0.11 appends the reason of the module's last reset; 0.10 and older send 22 bytes
        report_.resetReason = proto.readAvailable() >= 1 ? int(proto.read<U1>()) : -1;
        // 0.14 appends the requests the module could not queue (bytes 23-26, since its start)
        report_.lostValid = proto.readAvailable() >= 4;
        report_.lostRequests = report_.lostValid ? proto.read<U4>() : 0;
        return respOk;
    }
    if (proto.ver() == v2) {
        if (proto.readAvailable() < 2) {
            return respErrorPayload;
        }
        const int index = proto.read<U1>();
        const int total = proto.read<U1>();
        if (total == 0) {                       // nothing around: a single {0, 0} frame
            networks_.clear();
            scanTotal_ = 0;
            scanFilled_ = 0;
            scanFresh_ = false;
            scanPending_ = false;
            return respOk;
        }
        if (index >= total || proto.readAvailable() < 11) {
            return respErrorPayload;
        }
        if (scanFresh_ || scanTotal_ != total) {   // first frame of this scan
            scanFresh_ = false;
            scanTotal_ = total;
            scanFilled_ = 0;
            networks_ = QVector<Network>(total);
        }
        Network n;
        n.valid = true;
        n.rssi = proto.read<S1>();
        n.channel = proto.read<U1>();
        n.security = proto.read<U1>();
        n.flags = proto.read<U1>();
        n.bssid = readMac(proto);
        n.ssid = readSsid(proto);
        if (!networks_[index].valid) {
            ++scanFilled_;
        }
        networks_[index] = n;
        if (scanFilled_ >= total) {
            scanPending_ = false;
        }
        return respOk;
    }
    if (proto.ver() == v5) {
        if (proto.readAvailable() < 10) {
            return respErrorPayload;
        }
        relay_.valid = true;
        relay_.mode = proto.read<U1>();
        relay_.addr = proto.read<U1>();
        relay_.ip = readAddr(proto);
        relay_.rport = proto.read<U2>();
        relay_.lport = proto.read<U2>();
        relaySupport_ = 1;
        return respOk;
    }
    if (proto.ver() == v7) {
        if (proto.readAvailable() < 8) {
            return respErrorPayload;
        }
        radio_.valid = true;
        radio_.powerQ = proto.read<U1>();
        radio_.protocols = proto.read<U1>();
        radio_.bandwidth = proto.read<U1>();
        radio_.powerSave = proto.read<U1>();
        radio_.powerNowQ = proto.read<U1>();
        radio_.phyNow = proto.read<U1>();
        radio_.protocolsNow = proto.read<U1>();
        radio_.bandwidthNow = proto.read<U1>();
        radioSupport_ = 1;
        return respOk;
    }
    if (proto.ver() == v6) {
        if (proto.readAvailable() < 47) {
            return respErrorPayload;
        }
        relayStats_.valid = true;
        relayStats_.state = proto.read<U1>();
        relayStats_.peerIp = readAddr(proto);
        relayStats_.peerPort = proto.read<U2>();
        relayStats_.upFrames = proto.read<U4>();
        relayStats_.upBytes = proto.read<U4>();
        relayStats_.upPackets = proto.read<U4>();
        relayStats_.upDrops = proto.read<U4>();
        relayStats_.downPackets = proto.read<U4>();
        relayStats_.downBytes = proto.read<U4>();
        relayStats_.downFrames = proto.read<U4>();
        relayStats_.downDrops = proto.read<U4>();
        relayStats_.localFrames = proto.read<U4>();
        relayStats_.tcpConnects = proto.read<U4>();
        relaySupport_ = 1;
        return respOk;
    }
    if (proto.ver() == v4) {
        if (proto.readAvailable() < 2) {
            return respErrorPayload;
        }
        const int index = proto.read<U1>();
        const int total = proto.read<U1>();
        if (total == 0) {
            saved_.clear();
            savedTotal_ = 0;
            return respOk;
        }
        if (index >= total) {
            return respErrorPayload;
        }
        if (savedTotal_ != total || saved_.size() != total) {
            savedTotal_ = total;
            saved_ = QStringList();
            for (int i = 0; i < total; ++i) {
                saved_.append(QString());
            }
        }
        saved_[index] = readSsid(proto);
        return respOk;
    }
    return respErrorVersion;
}

void IDBinWifi::scan() {
    scanPending_ = true;
    scanStartedMs_ = QDateTime::currentMSecsSinceEpoch();
    // The last scan's networks stay until this one's first result replaces them: a view that scans again and
    // again (the Channels view's Live) does not go blank every few seconds.
    scanFresh_ = true;
    scanFilled_ = 0;
    simpleRequest(v2);
}

bool IDBinWifi::expireScan(int64_t nowMs) {
    if (!scanPending_ || nowMs - scanStartedMs_ < scanTimeoutMs) {
        return false;
    }
    scanPending_ = false;
    return true;
}

bool IDBinWifi::connectTo(const QString& ssid, const QString& password, bool save) {
    const QByteArray s = ssid.toUtf8();
    const QByteArray p = password.toUtf8();
    if (s.isEmpty() || s.size() > 32 || p.size() > 64) {
        return false;
    }
    ProtoBinOut out;
    out.create(SETTING, v3, id(), m_address);
    out.write<U1>(save ? 1 : 0);
    out.write<U1>(static_cast<U1>(s.size()));
    out.write(const_cast<char*>(s.constData()), static_cast<uint16_t>(s.size()));
    out.write<U1>(static_cast<U1>(p.size()));
    if (!p.isEmpty()) {
        out.write(const_cast<char*>(p.constData()), static_cast<uint16_t>(p.size()));
    }
    out.end();
    hashBinFrameOut(out);
    return true;
}

void IDBinWifi::action(uint8_t code, const QString& ssid) {
    ProtoBinOut out;
    out.create(SETTING, v4, id(), m_address);
    out.write<U1>(code);
    if (code == ActForget) {
        const QByteArray s = ssid.toUtf8().left(32);
        out.write<U1>(static_cast<U1>(s.size()));
        if (!s.isEmpty()) {
            out.write(const_cast<char*>(s.constData()), static_cast<uint16_t>(s.size()));
        }
    }
    out.end();
    hashBinFrameOut(out);
}

bool IDBinWifi::setRelay(uint8_t mode, uint8_t addr, const QString& ip, uint16_t rport, uint16_t lport) {
    const QStringList parts = ip.trimmed().split('.');
    if (parts.size() != 4) {
        return false;
    }
    uint8_t octets[4];
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        const int v = parts[i].toInt(&ok);
        if (!ok || v < 0 || v > 255) {
            return false;
        }
        octets[i] = static_cast<uint8_t>(v);
    }
    ProtoBinOut out;
    out.create(SETTING, v5, id(), m_address);
    out.write<U1>(mode);
    out.write<U1>(addr);
    out.write(octets, 4);
    out.write<U2>(rport);
    out.write<U2>(lport);
    out.end();
    hashBinFrameOut(out);
    return true;
}

void IDBinWifi::setRadio(uint8_t powerQ, uint8_t protocols, uint8_t bandwidth, uint8_t powerSave) {
    ProtoBinOut out;
    out.create(SETTING, v7, id(), m_address);
    out.write<U1>(powerQ);
    out.write<U1>(protocols);
    out.write<U1>(bandwidth);
    out.write<U1>(powerSave);
    out.end();
    hashBinFrameOut(out);
}

void IDBinWifi::setReportPeriod(uint16_t ms) {
    ProtoBinOut out;
    out.create(SETTING, v1, id(), m_address);
    out.write<U2>(ms);
    out.end();
    hashBinFrameOut(out);
}

void IDBinWifi::reset() {
    status_ = Status();
    report_ = Report();
    radio_ = Radio();
    radioSupport_ = -1;
    relay_ = Relay();
    relayStats_ = RelayStats();
    relaySupport_ = -1;
    networks_.clear();
    saved_.clear();
    scanTotal_ = -1;
    scanFilled_ = 0;
    scanFresh_ = false;
    savedTotal_ = -1;
    scanPending_ = false;
}

QString IDBinWifiNet::addrText(quint32 a) {
    return QStringLiteral("%1.%2.%3.%4").arg((a >> 24) & 0xFF).arg((a >> 16) & 0xFF).arg((a >> 8) & 0xFF).arg(a & 0xFF);
}

bool IDBinWifiNet::addrValue(const QString& s, quint32* out) {
    const QStringList parts = s.trimmed().split('.');
    if (parts.size() != 4) {
        return false;
    }
    quint32 v = 0;
    for (const QString& p : parts) {
        bool ok = false;
        const int b = p.toInt(&ok);
        if (!ok || b < 0 || b > 255) {
            return false;
        }
        v = (v << 8) | quint32(b);
    }
    *out = v;
    return true;
}

quint32 IDBinWifiNet::readAddr(FrameParser& proto) {
    quint32 v = 0;
    for (int i = 0; i < 4; ++i) {
        v = (v << 8) | proto.read<U1>();
    }
    return v;
}

void IDBinWifiNet::writeAddr(ProtoBinOut& out, quint32 a) {
    out.write<U1>(U1((a >> 24) & 0xFF));
    out.write<U1>(U1((a >> 16) & 0xFF));
    out.write<U1>(U1((a >> 8) & 0xFF));
    out.write<U1>(U1(a & 0xFF));
}

Resp IDBinWifiNet::parsePayload(FrameParser &proto) {
    switch (proto.ver()) {
    case v0:
        if (proto.readAvailable() < 2) return respErrorPayload;
        role_.saved = proto.read<U1>();
        role_.running = proto.read<U1>();
        role_.valid = true;
        break;
    case v1: {
        if (proto.readAvailable() < 6) return respErrorPayload;
        Ap a;
        a.channel = proto.read<U1>();
        a.hidden = proto.read<U1>();
        a.maxClients = proto.read<U1>();
        a.auth = proto.read<U1>();
        int len = proto.read<U1>();
        len = qMin(len, int(proto.readAvailable()));
        QByteArray raw(len, 0);
        if (len > 0) proto.read(reinterpret_cast<uint8_t*>(raw.data()), int16_t(len));
        a.ssid = QString::fromUtf8(raw);
        a.valid = true;
        ap_ = a;                                  // the trailing password length is always 0
        break;
    }
    case v2:
        if (proto.readAvailable() < 20) return respErrorPayload;
        ipCfg_.ip = readAddr(proto);
        ipCfg_.mask = readAddr(proto);
        ipCfg_.dhcp = proto.read<U1>() != 0;
        ipCfg_.poolStart = readAddr(proto);
        ipCfg_.poolEnd = readAddr(proto);
        ipCfg_.leaseMin = proto.read<U2>();
        ipCfg_.offer = proto.read<U1>();
        ipCfg_.valid = true;
        break;
    case v3: {
        if (proto.readAvailable() < 18) return respErrorPayload;
        const int i = proto.read<U1>();
        if (i < 0 || i >= kLines) return respErrorPayload;
        Line& l = lines_[i];
        l.mode = proto.read<U1>();
        l.dest = proto.read<U1>();
        l.ip = readAddr(proto);
        l.rport = proto.read<U2>();
        l.lport = proto.read<U2>();
        l.baud = proto.read<U4>();
        l.txPin = proto.read<S1>();
        l.rxPin = proto.read<S1>();
        l.uart = proto.read<U1>();
        l.valid = true;
        lastLine_ = i;
        break;
    }
    case v4: {
        if (proto.readAvailable() < 57) return respErrorPayload;
        const int i = proto.read<U1>();
        if (i < 0 || i >= kLines) return respErrorPayload;
        LineStats& st = stats_[i];
        st.state = proto.read<U1>();
        st.peerIp = readAddr(proto);
        st.peerPort = proto.read<U2>();
        st.senders = proto.read<U1>();
        for (uint32_t& c : st.counters) c = proto.read<U4>();
        st.valid = true;
        break;
    }
    case v5: {
        if (proto.readAvailable() < 2) return respErrorPayload;
        const int index = proto.read<U1>();
        const int total = proto.read<U1>();
        if (total == 0) {
            clients_.clear();
            clientsTotal_ = 0;
            break;
        }
        if (index >= total || proto.readAvailable() < 11) return respErrorPayload;
        if (clientsTotal_ != total || index == 0) {
            clientsTotal_ = total;
            clients_.clear();
        }
        Client c;
        uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
        proto.read(mac, 6);
        c.mac = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(mac), 6).toHex(':'));
        c.ip = readAddr(proto);
        c.rssi = proto.read<S1>();
        clients_.append(c);
        break;
    }
    case v6:
        if (proto.readAvailable() < 1) return respErrorPayload;
        ownAddress_ = proto.read<U1>();
        break;
    case v7: {                                    // firmware 0.12: port information, one page per frame
        const int16_t n = proto.readAvailable();
        if (n < 2) return respErrorPayload;
        QByteArray raw(n, 0);
        proto.read(reinterpret_cast<uint8_t*>(raw.data()), n);
        if (ports_.apply(reinterpret_cast<const uint8_t*>(raw.constData()), int(n), portClock_.elapsed()) < 0)
            return respErrorPayload;
        portSupport_ = 1;
        break;
    }
    default:
        return respErrorVersion;
    }
    support_ = 1;
    return respOk;
}

void IDBinWifiNet::requestPorts() {
    simpleRequest(v7);
}

void IDBinWifiNet::requestPortPage(uint8_t port, uint8_t page) {
    ProtoBinOut out;
    out.create(GETTING, v7, id(), m_address);
    out.write<U1>(port);
    out.write<U1>(page);
    out.end();
    emit binFrameOut(out);
}

Resp IDBinWifiSurvey::parsePayload(FrameParser &proto) {
    if (proto.ver() == v1)
        return parseLinkRate(proto);
    if (proto.ver() != v0)
        return respErrorVersion;
    if (proto.readAvailable() < 2)
        return respErrorPayload;
    const int index = proto.read<U1>();
    const int total = proto.read<U1>();
    if (total == 0) {                           // the module has no survey: a single {0, 0}
        channels_.clear();
        total_ = 0;
        filled_ = 0;
        fresh_ = false;
        running_ = false;
        return respOk;
    }
    if (index >= total || proto.readAvailable() < 12)
        return respErrorPayload;
    if (fresh_ || total_ != total) {            // first frame of this survey
        fresh_ = false;
        total_ = total;
        filled_ = 0;
        channels_ = QVector<Channel>(total);
    }
    Channel c;
    c.valid = true;
    c.channel = proto.read<U1>();
    c.dwellMs = proto.read<U2>();
    c.busyPermille = proto.read<U2>();
    c.frames = proto.read<U2>();
    c.transmitters = proto.read<U2>();
    c.strongest = proto.read<S1>();
    c.noiseDbm = proto.read<S1>();
    c.flags = proto.read<U1>();
    if (proto.readAvailable() >= 2) {           // 0.17: the own network's part of the airtime
        c.ownPermille = proto.read<U2>();
        c.ownValid = true;
    }
    const bool first = !channels_[index].valid;
    if (first)
        ++filled_;
    channels_[index] = c;
    if (first && filled_ == total_)
        ++completed_;
    if (filled_ >= total_)
        running_ = false;
    return respOk;
}

void IDBinWifiSurvey::start(uint16_t dwellMs, uint16_t mask) {
    running_ = true;
    fresh_ = true;
    dwellMs_ = dwellMs;
    startedMs_ = QDateTime::currentMSecsSinceEpoch();
    ProtoBinOut out;
    out.create(SETTING, v0, id(), m_address);
    appendKey(out);
    out.write<U2>(dwellMs);
    out.write<U2>(mask);
    out.end();
    hashBinFrameOut(out);
}

void IDBinWifiSurvey::reset() {
    channels_.clear();
    total_ = -1;
    filled_ = 0;
    running_ = false;
    fresh_ = false;
    ratePages_.clear();
    ratePeers_ = -1;
    rateFilled_ = 0;
    rateRunning_ = false;
    rateFresh_ = false;
}

void IDBinWifiSurvey::requestLinkRate(uint16_t windowMs) {
    rateRunning_ = true;
    rateFresh_ = true;
    rateWindowMs_ = windowMs;
    rateStartedMs_ = QDateTime::currentMSecsSinceEpoch();
    ProtoBinOut out;
    out.create(GETTING, v1, id(), m_address);
    out.write<U2>(windowMs);
    out.end();
    emit binFrameOut(out);
}

// A CONTENT v1 page kept raw; {0, 0} (no other end) ends the report at once.
Resp IDBinWifiSurvey::parseLinkRate(FrameParser &proto) {
    if (proto.readAvailable() < 2)
        return respErrorPayload;
    QByteArray page;
    while (proto.readAvailable() > 0)
        page.append(char(proto.read<U1>()));
    const int index = uint8_t(page[0]);
    const int total = uint8_t(page[1]);
    if (total == 0) {
        ratePages_.clear();
        ratePeers_ = 0;
        rateFilled_ = 0;
        rateFresh_ = false;
        rateRunning_ = false;
        ++rateCompleted_;
        return respOk;
    }
    if (index >= total || page.size() < 16)
        return respErrorPayload;
    if (rateFresh_ || ratePeers_ != total) {    // the first page of this report
        rateFresh_ = false;
        ratePeers_ = total;
        rateFilled_ = 0;
        ratePages_ = QVector<QByteArray>(total);
    }
    const bool first = ratePages_[index].isEmpty();
    ratePages_[index] = page;
    if (first && ++rateFilled_ == ratePeers_) {
        rateRunning_ = false;
        ++rateCompleted_;
    }
    return respOk;
}

void IDBinWifiNet::setRole(uint8_t role, bool reboot, bool forgetLines) {
    ProtoBinOut out;
    out.create(SETTING, v0, id(), m_address);
    appendKey(out);
    out.write<U1>(role);
    out.write<U1>(U1((reboot ? 1 : 0) | (forgetLines ? 2 : 0)));
    out.end();
    hashBinFrameOut(out);
}

bool IDBinWifiNet::setAp(uint8_t channel, bool hidden, uint8_t maxClients, uint8_t auth, const QString& ssid,
                         const QString& password) {
    const QByteArray s = ssid.toUtf8();
    const QByteArray p = password.toUtf8();
    // Channel 1-11: the driver's default country gives an access point no more (0.11 refuses 12-13)
    if (s.isEmpty() || s.size() > 32 || channel < 1 || channel > 11 || maxClients < 1 || maxClients > 10 || auth > ApWpa2Wpa3
        || (auth == ApOpen ? !p.isEmpty() : (!p.isEmpty() && (p.size() < 8 || p.size() > 63)))) {
        return false;
    }
    ProtoBinOut out;
    out.create(SETTING, v1, id(), m_address);
    appendKey(out);
    out.write<U1>(channel);
    out.write<U1>(hidden ? 1 : 0);
    out.write<U1>(maxClients);
    out.write<U1>(auth);
    out.write<U1>(U1(s.size()));
    out.write(const_cast<char*>(s.constData()), uint16_t(s.size()));
    out.write<U1>(U1(p.size()));
    if (!p.isEmpty()) out.write(const_cast<char*>(p.constData()), uint16_t(p.size()));
    out.end();
    hashBinFrameOut(out);
    return true;
}

bool IDBinWifiNet::setIpCfg(const QString& ip, const QString& mask, bool dhcp, const QString& poolStart,
                            const QString& poolEnd, uint16_t leaseMin, bool offerGateway, bool offerDns) {
    quint32 a = 0, m = 0, ps = 0, pe = 0;
    if (!addrValue(ip, &a) || !addrValue(mask, &m) || !addrValue(poolStart, &ps) || !addrValue(poolEnd, &pe)) {
        return false;
    }
    ProtoBinOut out;
    out.create(SETTING, v2, id(), m_address);
    appendKey(out);
    writeAddr(out, a);
    writeAddr(out, m);
    out.write<U1>(dhcp ? 1 : 0);
    writeAddr(out, ps);
    writeAddr(out, pe);
    out.write<U2>(leaseMin);
    out.write<U1>(U1((offerGateway ? 1 : 0) | (offerDns ? 2 : 0)));
    out.end();
    hashBinFrameOut(out);
    return true;
}

bool IDBinWifiNet::setLine(uint8_t line, uint8_t mode, uint8_t dest, const QString& ip, uint16_t rport, uint16_t lport,
                           uint32_t baud, int8_t txPin, int8_t rxPin) {
    quint32 a = 0;
    if (line >= kLines || mode > LineTcp || dest > DestBroadcast || !addrValue(ip, &a)) {
        return false;
    }
    ProtoBinOut out;
    out.create(SETTING, v3, id(), m_address);
    appendKey(out);
    out.write<U1>(line);
    out.write<U1>(mode);
    out.write<U1>(dest);
    writeAddr(out, a);
    out.write<U2>(rport);
    out.write<U2>(lport);
    out.write<U4>(baud);
    out.write<S1>(txPin);
    out.write<S1>(rxPin);
    out.end();
    hashBinFrameOut(out);
    return true;
}

void IDBinWifiNet::setOwnAddress(uint8_t addr) {
    ProtoBinOut out;
    out.create(SETTING, v6, id(), m_address);
    appendKey(out);
    out.write<U1>(addr);
    out.end();
    hashBinFrameOut(out);
}

void IDBinWifiNet::reset() {
    support_ = -1;
    role_ = RoleInfo();
    ap_ = Ap();
    ipCfg_ = IpCfg();
    for (int i = 0; i < kLines; ++i) {
        lines_[i] = Line();
        stats_[i] = LineStats();
    }
    clients_.clear();
    clientsTotal_ = -1;
    ownAddress_ = -1;
    lastLine_ = -1;
    portSupport_ = -1;
    ports_.reset();
}

Resp IDBinWifiRf::parsePayload(FrameParser &proto) {
    // A SETTING seen on the link (its key where the state would be) is not the module's state.
    if (proto.type() != CONTENT)
        return respErrorType;
    if (proto.ver() != v0)
        return respErrorVersion;
    if (proto.readAvailable() < 3)
        return respErrorPayload;
    const uint8_t on = proto.read<U1>();
    const uint16_t left = proto.read<U2>();
    if (on > 1)
        return respErrorPayload;
    state_.valid = true;
    state_.on = on == 1;
    state_.secondsLeft = state_.on ? 0 : left;
    return respOk;
}

void IDBinWifiRf::send(uint8_t on, uint16_t seconds) {
    ProtoBinOut out;
    out.create(SETTING, v0, id(), m_address);
    appendKey(out);
    out.write<U1>(on);
    out.write<U2>(seconds);
    out.end();
    hashBinFrameOut(out);
}

void IDBinWifiRf::switchOff(uint16_t seconds) {
    send(0, seconds);
}

void IDBinWifiRf::switchOn() {
    send(1, 0);
}

Resp IDBinVersion::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
        m_boardVersionMinor = proto.read<U1>();
        m_boardVersion = (BoardVersion)proto.read<U1>();
        proto.read<U2>();
        proto.read<U2>();
        proto.read<U2>();
        proto.read<U4>();
        proto.read<U2>();
        m_serialNumber = proto.read<U4>();
    }  else if(proto.ver() == v1) {
        if(proto.readAvailable() == 12) {
            _uid.resize(12);
            proto.read((uint8_t*)_uid.data(), 12);
        } else {
        }
    } else if(proto.ver() == v2) {
        _bootMode = proto.read<U1>();
        m_boardVersionMinor = proto.read<U1>();
        m_boardVersion = (BoardVersion)proto.read<U1>();
        _bootVersionMinor = proto.read<U1>();
        _bootVersion = proto.read<U1>();
        proto.read<U2>();
        _fwVersionMinor = proto.read<U1>();
        _fwVersion = proto.read<U1>();
    } else {
        return respErrorVersion;
    }

    return respOk;
}

Resp IDBinMark::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
        m_mark = proto.read<U1>();
    } else {
        return respErrorVersion;
    }

    return respOk;
}

void IDBinMark::setMark() {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    appendKey(id_out);
    id_out.end();

    emit binFrameOut(id_out);
}


Resp IDBinFlash::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0 || proto.ver() == v1 || proto.ver() == v2) {
        if (!checkKeyConfirm(proto.read<U4>())) {
            return respErrorKey;
        }
    } else {
        return respErrorVersion;
    }

    return respOk;
}

void IDBinFlash::flashing() {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    appendKey(id_out);
    id_out.end();
    emit binFrameOut(id_out);
}


void IDBinFlash::restore() {
    ProtoBinOut id_out;
    id_out.create(SETTING, v1, id(), m_address);
    appendKey(id_out);
    id_out.end();
    emit binFrameOut(id_out);
}

void IDBinFlash::erase() {
    ProtoBinOut id_out;
    id_out.create(SETTING, v2, id(), m_address);
    appendKey(id_out);
    id_out.end();
    emit binFrameOut(id_out);
}


Resp IDBinBoot::parsePayload(FrameParser &proto) {
    if (proto.ver() == v0 || proto.ver() == v1) {
        if (!checkKeyConfirm(proto.read<U4>())) {
            return respErrorKey;
        }
    } else {
        return respErrorVersion;
    }

    return respOk;
}

void IDBinBoot::reboot() {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    appendKey(id_out);
    id_out.end();
    emit binFrameOut(id_out);
}

void IDBinBoot::runFW() {
    ProtoBinOut id_out;
    id_out.create(SETTING, v1, id(), m_address);
    appendKey(id_out);
    id_out.end();
    emit binFrameOut(id_out);
}

Resp IDBinUpdate::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
        _progress = proto.read<ID_UPGRADE_V0>();
    } else {
        return respErrorVersion;
    }

    return respOk;
}

void IDBinUpdate::setUpdate(QByteArray fw) {
    _fw = fw;
    _currentFwOffset = 0;
    _currentNumPacket = 1;
}

QByteArray IDBinUpdate::getUpdate() const
{
    return _fw;
}

bool IDBinUpdate::putUpdate(bool is_auto_offset) {
    uint16_t len_part = _packetSize;

    if(len_part > availSend()) {
        len_part = (uint16_t)availSend();
    }

    if(len_part == 0) {
        return false;
    }

    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U2>(_currentNumPacket);

    for(uint16_t i = 0; i < len_part; i++) {
        id_out.write<U1>((U1)_fw[i + _currentFwOffset]);
    }

    if(is_auto_offset) {
        _currentFwOffset += len_part;
        _currentNumPacket++;
    }

    id_out.end();
    emit binFrameOut(id_out);

    return true;
}


Resp IDBinVoltage::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
        uint8_t ch_id = proto.read<U1>();
        int32_t u_v = proto.read<S4>();
        _v[ch_id] =  float(u_v);
        _last_id = ch_id;
        _last_raw = u_v;
    } else {
        return respErrorVersion;
    }

    return respOk;
}

Resp IDBinPreset::parsePayload(FrameParser &proto) {
    if (proto.ver() != v0) {
        return respErrorVersion;
    }
    if (proto.readAvailable() < kFrameBytes) {
        return respErrorPayload;
    }
    const uint8_t index = proto.read<U1>();
    if (index >= kSlots) {
        return respErrorPayload;
    }
    Slot s;
    const uint8_t flags = proto.read<U1>();
    s.known = true;
    s.used = (flags & 0x01) != 0;
    s.factory = (flags & 0x02) != 0;
    s.freqKhz = proto.read<U2>();
    s.pulses = proto.read<U1>();
    s.boost = proto.read<U1>();
    s.dsp = proto.read<U1>();
    s.cells = proto.read<U2>();
    s.cellMm = proto.read<U2>();
    s.offset = proto.read<U2>();
    s.periodMs = proto.read<U4>();
    char name[kNameBytes] = {};
    proto.read(reinterpret_cast<uint8_t*>(name), kNameBytes);
    s.name = QString::fromUtf8(name, int(qstrnlen(name, kNameBytes)));
    slots_[index] = s;
    lastSlot_ = index;
    return respOk;
}

QByteArray IDBinPreset::nameBytes(const QString& name) {
    QByteArray b = name.trimmed().toUtf8();
    if (b.size() > kNameBytes) {
        int n = kNameBytes;   // b[n] is the first byte left out; a continuation byte there means its character
        while (n > 0 && (uint8_t(b[n]) & 0xC0) == 0x80) {   // began before the cut: leave it out whole
            --n;
        }
        b.truncate(n);
    }
    return b;
}

void IDBinPreset::recall(int slot) {
    if (slot < 0 || slot >= kSlots) {
        return;
    }
    ProtoBinOut out;
    out.create(SETTING, v0, id(), m_address);
    out.write<U1>(U1(slot));
    out.end();
    emit binFrameOut(out);
}

void IDBinPreset::save(int slot, const QString& name) {
    if (slot < 0 || slot >= kSlots) {
        return;
    }
    QByteArray n = nameBytes(name);
    ProtoBinOut out;
    out.create(SETTING, v1, id(), m_address);
    out.write<U1>(U1(slot));
    out.write<U4>(slot <= 1 ? m_key : U4(0));   // the key: the factory slots 0..1 only (the contract)
    out.write(n.data(), uint16_t(n.size()));
    out.writeZero(uint16_t(kNameBytes - n.size()));
    out.end();
    emit binFrameOut(out);
}

void IDBinPreset::remove(int slot) {
    if (slot < 0 || slot >= kSlots) {
        return;
    }
    ProtoBinOut out;
    out.create(SETTING, v2, id(), m_address);
    out.write<U1>(U1(slot));
    appendKey(out);
    out.end();
    emit binFrameOut(out);
}

Resp IDBinDVLMode::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {

    } else {
        return respErrorVersion;
    }

    return respOk;
}

void IDBinDVLMode::setModes(bool ismode1, bool ismode2, bool ismode3, bool ismode4, float range_mode4) {
    ProtoBinOut id_out;
    id_out.create(SETTING, v0, id(), m_address);
    id_out.write<U1>(1);
    id_out.write<U1>(0);
    id_out.write<U1>(0);
    id_out.write<U1>(0);

    DVLModeSetup mode1, mode2, mode3, mode4;
    mode1.id = 1;
    mode2.id = 2;
    mode3.id = 3;
    mode4.id = 4;
    mode1.selection = ismode1;
    mode2.selection = ismode2;
    mode3.selection = ismode3;
    mode4.selection = ismode4;
    mode4.stop = range_mode4;
    id_out.write<DVLModeSetup>(mode1);
    id_out.write<DVLModeSetup>(mode2);
    id_out.write<DVLModeSetup>(mode3);
    id_out.write<DVLModeSetup>(mode4);

    id_out.end();
    emit binFrameOut(id_out);
}

void IDBinUsblSolution::projectToSolution(const AcousticNavSolution& src) {
    _usblSolution = {};
    _usblSolution.id = src.address;
    _usblSolution.cmd_id = src.cmd_id;
    _usblSolution.timestamp_us = src.timestamp_us;
    _usblSolution.carrier_counter = src.carrier_counter;
    _usblSolution.distance_m = src.distance;
    _usblSolution.azimuth_deg = src.acousticAzimuth;
    _usblSolution.beacon_latitude = src.lat;
    _usblSolution.beacon_longitude = src.lon;
    _usblSolution.beacon_depth = src.depth;
    _usblSolution.usbl_yaw = src.heading;
    _usblSolution.usbl_latitude = src.baseLat;
    _usblSolution.usbl_longitude = src.baseLon;
}

void IDBinUsblSolution::projectToSolution(const BaseToBeacon& src) {
    _usblSolution = {};
    _usblSolution.id = src.address;
    _usblSolution.cmd_id = src.cmd_id;
    _usblSolution.timestamp_us = src.timestamp_us;
    _usblSolution.carrier_counter = src.carrier_counter;
    _usblSolution.distance_m = src.beaconDistance;
    _usblSolution.azimuth_deg = src.acousticAzimuth;
    _usblSolution.beacon_depth = src.beaconD;
    _usblSolution.beacon_latitude = src.beaconLat;
    _usblSolution.beacon_longitude = src.beaconLon;
    _usblSolution.beacon_n_m = src.beaconN;
    _usblSolution.beacon_e_m = src.beaconE;
    _usblSolution.usbl_yaw = src.antennaYaw;
    _usblSolution.usbl_latitude = src.antennaLat;
    _usblSolution.usbl_longitude = src.antennaLon;
}

Resp IDBinUsblSolution::parsePayload(FrameParser &proto) {
    if(proto.ver() == v0) {
        _usblSolution = proto.read<UsblSolution>();
        _lastPayloadKind = PayloadKind::Solution;
        qInfo("USBL addr: %d, cmd: %d, d: %f, a: %f, e: %f, y: %f", _usblSolution.id, _usblSolution.cmd_id,
              _usblSolution.distance_m, _usblSolution.azimuth_deg, _usblSolution.elevation_deg, _usblSolution.usbl_yaw);
    } else if(proto.ver() == v1) {
        // v1 is overloaded: a full nav solution, or a 4-byte beacon activation
        // response. Length is the only discriminator.
        if(proto.readAvailable() >= (int16_t)sizeof(AcousticNavSolution)) {
            _acousticNavSolution = proto.read<AcousticNavSolution>();
            _lastPayloadKind = PayloadKind::AcousticNav;
            projectToSolution(_acousticNavSolution);
        } else if(proto.readAvailable() >= (int16_t)sizeof(BeaconActivationResponce)) {
            _beaconResponcel = proto.read<BeaconActivationResponce>();
            _lastPayloadKind = PayloadKind::BeaconActivation;
            qInfo("Beacon responce: %d", _beaconResponcel.id);
        } else {
            return respErrorPayload;
        }
    } else if(proto.ver() == v2) {
        if(proto.readAvailable() < (int16_t)sizeof(BaseToBeacon)) {
            return respErrorPayload;
        }
        _baseToBeacon = proto.read<BaseToBeacon>();
        _lastPayloadKind = PayloadKind::BaseToBeacon;
        projectToSolution(_baseToBeacon);
    } else {
        return respErrorVersion;
    }

    return respOk;
}

void IDBinUsblSolution::askBeacon(USBLRequestBeacon ask) {
    ProtoBinOut req_out;
    req_out.create(GETTING, Version::v0, id(), m_address);
    req_out.write<USBLRequestBeacon>(ask);
    req_out.end();

    emit binFrameOut(req_out);
}

void IDBinUsblSolution::enableBeaconOnce(float timeout) {
    ProtoBinOut req_out;
    req_out.create(GETTING, Version::v1, id(), m_address);
    req_out.write<F4>(timeout);
    req_out.end();

    emit binFrameOut(req_out);
}

Resp IDBinUsblDiag::parsePayload(FrameParser& proto)
{
    if (proto.ver() != UsblSignalDiag::getVer()) {
        return respErrorVersion;
    }

    if (proto.readAvailable() < static_cast<int16_t>(sizeof(UsblSignalDiag))) {
        return respErrorPayload;
    }

    data_ = proto.read<UsblSignalDiag>();
    valid_ = true;
    return respOk;
}



Resp IDBinUsblControl::parsePayload(FrameParser& proto)
{
    Q_UNUSED(proto)

    return respOk;
}

uint16_t IDBinUsblControl::pingRequest(uint32_t timeout_us, uint8_t address, uint8_t cmd_id) {
    return pingRequest(timeout_us, address, cmd_id, 20000, QByteArray());
}

uint16_t IDBinUsblControl::send(ProtoBinOut& frame) {
    emit binFrameOut(frame);

    const uint8_t* bytes = frame.frame();
    const int checked_end = frame.frameLen() - 2;
    uint8_t check1 = 0, check2 = 0;
    for(int i = 2; i < checked_end; i++) {
        check1 += (i == 2) ? 0 : bytes[i];
        check2 += check1;
    }
    lastRelayedChecksum_ = (uint16_t)(check1 | (check2 << 8));
    return frame.checksum();
}

uint16_t IDBinUsblControl::pingRequest(uint32_t timeout_us, uint8_t address, uint8_t cmd_id, uint32_t reply_distance_mm, const QByteArray& payload, int payload_bits) {
    ProtoBinOut ping_req;
    ping_req.create(SETTING, USBLPingRequest::getVer(), id(), m_address);

    int max_payload_bytes = ping_req.frameSpaceAvail() - (int)sizeof(USBLPingRequest);
    if(max_payload_bytes < 0) {
        max_payload_bytes = 0;
    }
    int payload_bytes = qMin((int)payload.size(), max_payload_bytes);
    int bits = payload_bytes * 8;
    if(payload_bits >= 0 && payload_bits < bits) {
        bits = payload_bits;
        payload_bytes = (bits + 7) / 8;
    }

    USBLPingRequest req;
    req.trigger_timeout_us = timeout_us;
    req.address = address;
    req.cmd_id = cmd_id;
    req.reply_distance_mm = reply_distance_mm;
    req.function = bits > 0 ? USBLPingRequest::FunctionBitArray : USBLPingRequest::FunctionDefault;
    req.payload_bit_length = (uint16_t)bits;

    ping_req.write<USBLPingRequest>(req);
    if(bits > 0) {
        ping_req.write((void*)payload.constData(), (uint16_t)payload_bytes);
    }
    ping_req.end();

    return send(ping_req);
}

uint16_t IDBinUsblControl::setTransponderEnable(uint32_t timeout_us) {
    ProtoBinOut ping_resp_t;
    ping_resp_t.create(SETTING, USBLResponseTimeout::getVer(), id(), m_address);
    USBLResponseTimeout req = {timeout_us};
    ping_resp_t.write<USBLResponseTimeout>(req);
    ping_resp_t.end();

    return send(ping_resp_t);
}

uint16_t IDBinUsblControl::setResponseTimeout(uint32_t timeout_us) {
    return setTransponderEnable(timeout_us);
}

uint16_t IDBinUsblControl::setMonitorConfig(const USBLMonitorConfig& cfg) {
    ProtoBinOut monitor_cfg;
    monitor_cfg.create(SETTING, USBLMonitorConfig::getVer(), id(), m_address);
    monitor_cfg.write<USBLMonitorConfig>(cfg);
    monitor_cfg.end();

    return send(monitor_cfg);
}

uint16_t IDBinUsblControl::setResponseAddressFilter(const std::array<uint8_t, 8>& addresses) {
    ProtoBinOut ping_resp_a;
    ping_resp_a.create(SETTING, USBLResponseAddressFilter::getVer(), id(), m_address);
    USBLResponseAddressFilter req;
    for(size_t i = 0; i < addresses.size(); ++i) {
        req.address[i] = addresses[i];
    }
    ping_resp_a.write<USBLResponseAddressFilter>(req);
    ping_resp_a.end();

    return send(ping_resp_a);
}

uint16_t IDBinUsblControl::setResponseAddressFilter(uint8_t address) {
    std::array<uint8_t, 8> addresses;
    addresses.fill(0xFF);
    addresses[0] = address;
    return setResponseAddressFilter(addresses);
}

uint16_t IDBinUsblControl::setCmdConfig(const USBLCmdConfig& cfg, const QByteArray& sendingPayload, int sending_bits) {
    ProtoBinOut proto_cfg;
    proto_cfg.create(SETTING, USBLCmdConfig::getVer(), id(), m_address);

    int max_payload_bytes = proto_cfg.frameSpaceAvail() - (int)sizeof(USBLCmdConfig);
    if(max_payload_bytes < 0) {
        max_payload_bytes = 0;
    }

    const bool sender_is_bit_array = cfg.sender_function == USBLCmdConfig::FunctionBitArray;
    const QByteArray payload = sender_is_bit_array ? sendingPayload : QByteArray();
    int payload_bytes = qMin((int)payload.size(), max_payload_bytes);
    int bits = payload_bytes * 8;
    if(sending_bits >= 0 && sending_bits < bits) {
        bits = sending_bits;
        payload_bytes = (bits + 7) / 8;
    }

    USBLCmdConfig cfg_out = cfg;
    cfg_out.sending_bit_length = sender_is_bit_array ? (uint16_t)bits : 0;

    proto_cfg.write<USBLCmdConfig>(cfg_out);
    if(payload_bytes > 0) {
        proto_cfg.write((void*)payload.constData(), (uint16_t)payload_bytes);
    }
    proto_cfg.end();

    return send(proto_cfg);
}

Resp IDBinModemSolution::parsePayload(FrameParser &proto) {
    if(proto.ver() != v0) {
        return respErrorVersion;
    }

    if(proto.readAvailable() < (int16_t)sizeof(ModemSolutionHeader)) {
        return respErrorPayload;
    }

    _header = proto.read<ModemSolutionHeader>();
    const int byte_length = (_header.bit_length + 7) / 8;

    if(proto.readAvailable() < byte_length) {
        return respErrorPayload;
    }

    if(byte_length > 0) {
        _payload = QByteArray((char*)proto.read(byte_length), byte_length);
    } else {
        _payload.clear();
    }

    return respOk;
}
