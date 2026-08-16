// TTSEngine.cpp: CTTSEngine 的实现

#include "pch.h"
#include "TTSEngine.h"
#include "SpeechRestAPI.h"
#include "NetUtils.h"
#include "SpeechServiceConstants.h"
#include <VersionHelpers.h>
#include "RegKey.h"
#include "wrappers.h"
#include <charconv> 

// 【新增】初始化全局静态锁
std::mutex CTTSEngine::s_globalSynthMutex;

// CTTSEngine

static inline DWORD _GetTickCount()
{
#pragma warning (disable: 28159)
    return GetTickCount();
#pragma warning (default: 28159)
}

STDMETHODIMP CTTSEngine::SetObjectToken(ISpObjectToken* pToken) noexcept
{
    ScopeTracer tracer("TTS init: begin", "TTS init: end");
    try
    {
        CheckSapiHr(SpGenericSetObjectToken(pToken, m_cpToken));
        InitVoice();
        InitPhoneConverter();
        return S_OK;
    }
    catch (const std::bad_alloc&) { LogCritical("Out of memory"); return E_OUTOFMEMORY; }
    catch (const std::system_error& ex) { return OnException(ex, "TTS init: voice '{}' cannot be initialized: {}", pToken); }
    catch (const std::invalid_argument& ex) { return OnException(ex, "TTS init: voice '{}' cannot be initialized: {}", pToken); }
    catch (const std::exception& ex) { return OnException(ex, "TTS init: voice '{}' cannot be initialized: {}", pToken); }
    catch (...) { LogErr("TTS init: voice '{}' cannot be initialized: Unknown error", pToken); return E_FAIL; }
}

STDMETHODIMP CTTSEngine::Speak(DWORD /*dwSpeakFlags*/,
    REFGUID /*rguidFormatId*/,
    const WAVEFORMATEX* /*pWaveFormatEx*/,
    const SPVTEXTFRAG* pTextFragList,
    ISpTTSEngineSite* pOutputSite) noexcept
{
    ScopeTracer tracer("Speak: begin", "Speak: end");
    try
    {
        if (SP_IS_BAD_INTERFACE_PTR(pOutputSite) || SP_IS_BAD_READ_PTR(pTextFragList)) return E_INVALIDARG;
        if (!m_synthesizer && !m_restApi) return SPERR_UNINITIALIZED;

        std::unique_lock<std::mutex> lock(s_globalSynthMutex);

        if (m_lastCancellingFuture.valid())
        {
            while (m_lastCancellingFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout)
            {
                if (pOutputSite->GetActions() & SPVES_ABORT) return S_OK;
                lock.unlock(); Sleep(0); lock.lock();
            }
            m_lastCancellingFuture = {};
        }

        ULONGLONG eventInterests = 0;
        pOutputSite->GetEventInterest(&eventInterests);
        if (m_synthesizer) SetupSynthesizerEvents(eventInterests);
        else SetupRestAPIEvents(eventInterests);

        ScopeGuard siteDeleter([this]() { std::lock_guard lock(m_outputSiteMutex); m_pOutputSite = nullptr; });
        m_pOutputSite = pOutputSite;

        if (!BuildSSML(pTextFragList))
        {
            LogDebug("Speak: Built SSML with no speech: {}", m_ssml);
            FinishSimulatingBookmarkEvents(m_compensatedSilentBytes);
            return S_OK;
        }

        LogDebug("Speak: Built SSML: {}", m_ssml);

        m_compensatedSilenceWritten = false;
        m_compensatedSilentBytes = 0;
        m_lastSilentBytes = 0;
        m_thisSpeakStartedTicks = _GetTickCount();
        m_onlineDelayOptimization = !m_onlineVoiceName.empty() && RegOpenConfigKey().GetDword(L"EnableOnlineDelayOptimization");

        std::future<void> future;

        if (m_synthesizer)
        {
            std::wstring ssmlCopy = m_ssml;
            future = std::async(std::launch::async, [this, ssml = std::move(ssmlCopy)]() { 
                CheckSynthesisResult(m_synthesizer->SpeakSsml(ssml)); 
            });
        }
        else
        {
            future = m_restApi->SpeakAsync(m_ssml);
        }

        while (!(pOutputSite->GetActions() & SPVES_ABORT) && future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout)
        {
            if (pOutputSite->GetActions() & SPVES_SKIP) { LogWarn("Speak: Skipping not supported, ignored"); pOutputSite->CompleteSkip(0); }
            lock.unlock(); Sleep(10); lock.lock();
        }

        if (pOutputSite->GetActions() & SPVES_ABORT) 
        {
            LogDebug("Speak: Requested stop");
            if (m_synthesizer)
            {
                m_lastCancellingFuture = std::async(std::launch::async, [this, future = std::move(future)]() mutable {
                    while (!m_synthesizerStarted.load(std::memory_order_relaxed)) Sleep(0);
                    m_synthesizer->StopSpeakingAsync().wait();
                    future.wait();
                });
            }
            else m_restApi->Stop();
            m_lastSpeakCompletedTicks = 0;
        }
        else
        {
            lock.unlock(); future.get(); lock.lock();
            if (m_isEdgeVoice) FinishSimulatingBookmarkEvents(m_restApi->GetWaveBytesWritten() + m_compensatedSilentBytes - m_lastSilentBytes);
            m_lastSpeakCompletedTicks = _GetTickCount();
        }
        return S_OK;
    }
    catch (const std::bad_alloc&) { LogCritical("Out of memory"); return E_OUTOFMEMORY; }
    catch (const std::system_error& ex) { return OnException(ex, "Speak: {}"); }
    catch (const std::exception& ex) { return OnException(ex, "Speak: {}"); }
    catch (...) { LogErr("Speak: Unknown error"); return E_FAIL; }
}

STDMETHODIMP CTTSEngine::GetOutputFormat(const GUID* /*pTargetFormatId*/, const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
    GUID* pDesiredFormatId, WAVEFORMATEX** ppCoMemDesiredWaveFormatEx) noexcept
{
    return SpConvertStreamFormatEnum(SPSF_24kHz16BitMono, pDesiredFormatId, ppCoMemDesiredWaveFormatEx);
}

void CTTSEngine::InitPhoneConverter()
{
    LANGID lang = 0;
    HRESULT hr = SpGetLanguageFromToken(m_cpToken, &lang);
    if (FAILED(hr)) throw std::system_error(hr, sapi_category(), "Attribute 'Language' is missing");

    CComPtr<ISpDataKey> pAttrKey;
    CSpDynamicString locale;
    if (SUCCEEDED(m_cpToken->OpenKey(SPTOKENKEY_ATTRIBUTES, &pAttrKey)) && SUCCEEDED(pAttrKey->GetStringValue(L"Locale", &locale))) m_localeName = locale;
    else m_localeName = L"en-US";

    CheckSapiHr(SpCreatePhoneConverter(lang, nullptr, nullptr, &m_phoneConverter));
}

void CTTSEngine::InitVoice()
{
    CComPtr<ISpDataKey> pConfigKey;
    HRESULT hr = m_cpToken->OpenKey(L"NaturalVoiceConfig", &pConfigKey); 
    if (FAILED(hr)) throw std::system_error(hr, sapi_category(), "Subkey 'NaturalVoiceConfig' is missing");

    DWORD dwErrorMode;
    hr = pConfigKey->GetDWORD(L"ErrorMode", &dwErrorMode);
    if (FAILED(hr)) dwErrorMode = 0;
    m_errorMode = (ErrorMode)std::clamp(dwErrorMode, 0UL, 2UL);

    RegKey key = RegOpenConfigKey();

    if (IsWindows7OrGreater() || key.GetDword(L"ForceEnableAzureSpeechSDK"))
    {
        if (InitLocalVoice(pConfigKey)) return;
        if (key.GetDword(L"UseAzureSpeechSDKForAzureVoices") && InitCloudVoiceSynthesizer(pConfigKey)) return;
    }
    if (InitCloudVoiceRestAPI(pConfigKey)) return;
    
    throw std::invalid_argument("Invalid NaturalVoiceConfig configuration.");
}

inline static bool CheckHrNotFound(HRESULT hr) { if (hr == SPERR_NOT_FOUND) return true; CheckSapiHr(hr); return false; }

LSTATUS TryLoadAzureSpeechSDK();

bool CTTSEngine::InitLocalVoice(ISpDataKey* pConfigKey)
{
    if (TryLoadAzureSpeechSDK() != ERROR_SUCCESS) return false; 

    CSpDynamicString pszPath, pszKey;
    if (CheckHrNotFound(pConfigKey->GetStringValue(L"Path", &pszPath))) return false;

    if (!CheckHrNotFound(pConfigKey->GetStringValue(L"Key", &pszKey))) MergeIntoCoString(pszKey, L"Key:", pszKey.m_psz);
    else if (CheckHrNotFound(pConfigKey->GetStringValue(L"License", &pszKey))) return false;

    auto path = WStringToUTF8(pszPath.m_psz);
    auto config = EmbeddedSpeechConfig::FromPath(path);

    config->SetSpeechSynthesisOutputFormat(SpeechSynthesisOutputFormat::Riff24Khz16BitMonoPcm);
    config->SetProperty(PropertyId::SpeechServiceResponse_RequestSentenceBoundary, "true");
    config->SetProperty(PropertyId::SpeechServiceResponse_RequestPunctuationBoundary, "false");

    auto synthesizer = SpeechSynthesizer::FromConfig(config);
    auto result = synthesizer->GetVoicesAsync().get();
    if (result->Reason != ResultReason::VoicesListRetrieved || result->Voices.empty()) throw std::invalid_argument(UTF8ToAnsi(result->ErrorDetails));
    
    auto& voiceName = result->Voices[0]->Name;
    config->SetSpeechSynthesisVoice(voiceName, WStringToUTF8(pszKey.m_psz));

    if (m_errorMode == ErrorMode::ProbeForError)
    {
        auto testSynthesizer = SpeechSynthesizer::FromConfig(config, nullptr);
        CheckSynthesisResult(testSynthesizer->SpeakText("")); 
    }

    m_synthesizer = SpeechSynthesizer::FromConfig(config, AudioConfig::FromStreamOutput(AudioOutputStream::CreatePushStream(std::bind_front(&CTTSEngine::OnAudioData, this))));
    LogInfo("Local voice created: {}", voiceName);
    return true;
}

bool CTTSEngine::InitCloudVoiceSynthesizer(ISpDataKey* pConfigKey)
{
    if (LSTATUS stat = TryLoadAzureSpeechSDK(); stat != ERROR_SUCCESS) return false; 

    CSpDynamicString pszKey, pszRegion, pszVoice;
    if (CheckHrNotFound(pConfigKey->GetStringValue(L"Key", &pszKey)) || CheckHrNotFound(pConfigKey->GetStringValue(L"Region", &pszRegion)) || CheckHrNotFound(pConfigKey->GetStringValue(L"Voice", &pszVoice))) return false;

    auto config = SpeechConfig::FromSubscription(WStringToUTF8(pszKey.m_psz), WStringToUTF8(pszRegion.m_psz));
    config->SetSpeechSynthesisOutputFormat(SpeechSynthesisOutputFormat::Riff24Khz16BitMonoPcm);
    config->SetProperty(PropertyId::SpeechServiceResponse_RequestSentenceBoundary, "true");
    config->SetProperty(PropertyId::SpeechServiceResponse_RequestPunctuationBoundary, "false");

    auto proxy = GetProxyForUrl("https://" + WStringToUTF8(pszRegion.m_psz) + AZURE_TTS_HOST_AFTER_REGION);
    if (!proxy.empty())
    {
        auto url = ParseUrl(proxy);
        uint32_t port = 80;
        std::string portStr(url.port);
        if (!portStr.empty()) { try { port = std::stoul(portStr); } catch (...) { port = 80; } }
        config->SetProxy(std::string(url.host), port);
    }

    config->SetSpeechSynthesisVoiceName(WStringToUTF8(pszVoice.m_psz));
    m_onlineVoiceName = pszVoice;

    if (m_errorMode == ErrorMode::ProbeForError)
    {
        auto testSynthesizer = SpeechSynthesizer::FromConfig(config, nullptr);
        CheckSynthesisResult(testSynthesizer->SpeakText("")); 
    }

    m_synthesizer = SpeechSynthesizer::FromConfig(config, AudioConfig::FromStreamOutput(AudioOutputStream::CreatePushStream(std::bind_front(&CTTSEngine::OnAudioData, this))));
    LogInfo("Cloud voice (Azure Speech SDK) created: {}", pszVoice.m_psz);
    return true;
}

bool CTTSEngine::InitCloudVoiceRestAPI(ISpDataKey* pConfigKey)
{
    m_restApi = std::make_unique<SpeechRestAPI>();
    CSpDynamicString pszVoice, pszKey;
    if (CheckHrNotFound(pConfigKey->GetStringValue(L"Voice", &pszVoice))) return false;
    m_onlineVoiceName = pszVoice;

    DWORD dwValue = 0;
    if (!CheckHrNotFound(pConfigKey->GetDWORD(L"IsEdgeVoice", &dwValue))) m_isEdgeVoice = dwValue;
    
    if (CSpDynamicString pszWebsocketUrl; !CheckHrNotFound(pConfigKey->GetStringValue(L"WebsocketURL", &pszWebsocketUrl)))
    {
        CheckHrNotFound(pConfigKey->GetStringValue(L"Key", &pszKey));
        m_restApi->SetWebsocketUrl(pszKey ? WStringToUTF8(pszKey.m_psz) : "", WStringToUTF8(pszWebsocketUrl.m_psz));
    }
    else if (CSpDynamicString pszRegion; !CheckHrNotFound(pConfigKey->GetStringValue(L"Region", &pszRegion)))
    {
        if (CheckHrNotFound(pConfigKey->GetStringValue(L"Key", &pszKey))) return false;
        m_restApi->SetSubscription(WStringToUTF8(pszKey.m_psz), WStringToUTF8(pszRegion.m_psz));
    }
    else return false;

    LogInfo("Cloud voice (Rest API) created: {}", pszVoice.m_psz);
    return true;
}

template <typename SampleType>
static size_t GetTrailingSilenceLengthMono(BYTE* waveData, size_t length)
{
    constexpr size_t bytesPerSample = sizeof(SampleType);
    if (length < bytesPerSample) return 0;
    BYTE* p = waveData + (length - (length % bytesPerSample));
    SampleType smp;
    do {
        p -= bytesPerSample;
        memcpy(&smp, p, bytesPerSample);
        if (smp != SampleType()) return length - (p - waveData) - bytesPerSample;
    } while (p != waveData);
    return length;
}

int CTTSEngine::OnAudioData(uint8_t* data, uint32_t len)
{
    std::lock_guard lock(m_outputSiteMutex);
    if (!m_pOutputSite || len == 0 || data == nullptr) return len; 

    ULONG safeLen = static_cast<ULONG>(len);
    ULONG written = 0;

    if (m_onlineDelayOptimization)
    {
        if (!m_compensatedSilenceWritten)
        {
            DWORD currentTicks = _GetTickCount();
            DWORD passedMs = currentTicks - m_thisSpeakStartedTicks;  
            if (m_lastSpeakCompletedTicks != 0 && currentTicks - m_lastSpeakCompletedTicks < 5000)
            {
                DWORD silenceMs = m_lastSilentBytes / nWaveBytesPerMSec;  
                m_compensatedSilentBytes = silenceMs > passedMs ? (silenceMs - passedMs) * nWaveBytesPerMSec : 0;
                if (m_compensatedSilentBytes != 0)
                {
                    auto mem = std::make_unique<BYTE[]>(m_compensatedSilentBytes);  
                    memset(mem.get(), 0, m_compensatedSilentBytes); 
                    m_pOutputSite->Write(mem.get(), m_compensatedSilentBytes, &written);
                }
            }
            m_lastSilentBytes = 0;
            m_compensatedSilenceWritten = true;
        }

        ULONG silentBytes = static_cast<ULONG>(GetTrailingSilenceLengthMono<USHORT>(data, safeLen));
        if (silentBytes == safeLen)
        {
            if (m_lastSilentBytes < nWaveBytesPerMSec * 1000) { m_lastSilentBytes += silentBytes; return len; }
        }
        else
        {
            if (m_lastSilentBytes != 0)
            {
                auto mem = std::make_unique<BYTE[]>(m_lastSilentBytes);  
                memset(mem.get(), 0, m_lastSilentBytes);
                m_pOutputSite->Write(mem.get(), m_lastSilentBytes, &written);
            }
            m_lastSilentBytes = silentBytes;
        }
        safeLen = safeLen - silentBytes;
        if (safeLen == 0) return len;
    }

    // 【修复】加入重试次数限制，防止死循环导致程序彻底卡死
    uint8_t* pData = data;
    ULONG remaining = safeLen;
    int retryCount = 0;
    const int MAX_RETRIES = 50; // 最多等待 250ms

    while (remaining > 0 && retryCount < MAX_RETRIES)
    {
        if (m_pOutputSite->GetActions() & SPVES_ABORT) return 0; 

        written = 0;
        HRESULT hr = m_pOutputSite->Write(pData, remaining, &written);

        if (SUCCEEDED(hr))
        {
            if (written == 0 && remaining > 0) { Sleep(5); retryCount++; continue; }
            pData += written;
            remaining -= written;
            retryCount = 0; // 成功写入则重置计数器
        }
        else if (hr == SPERR_AUDIO_STOPPED || hr == E_FAIL) return 0; 
        else { Sleep(5); retryCount++; }
    }

    return len; 
}

void CTTSEngine::OnBookmark(uint64_t offsetTicks, const std::wstring& bookmark) { std::lock_guard lock(m_outputSiteMutex); if (!m_pOutputSite) return; SPEVENT ev = { 0 }; ev.ullAudioStreamOffset = WaveTicksToBytes(offsetTicks); ev.eEventId = SPEI_TTS_BOOKMARK; ev.elParamType = SPET_LPARAM_IS_STRING; ev.lParam = reinterpret_cast<LPARAM>(bookmark.c_str()); ev.wParam = _wtol(bookmark.c_str()); m_pOutputSite->AddEvents(&ev, 1); }
void CTTSEngine::OnBoundary(uint64_t audioOffsetTicks, uint32_t textOffset, uint32_t textLength, SPEVENTENUM boundaryType) { std::lock_guard lock(m_outputSiteMutex); if (!m_pOutputSite) return; SPEVENT ev = { 0 }; ev.ullAudioStreamOffset = WaveTicksToBytes(audioOffsetTicks); ev.eEventId = boundaryType; ev.elParamType = SPET_LPARAM_IS_UNDEFINED; ULONG offset = textOffset, length = textLength; MapTextOffset(offset, length); ev.lParam = offset; ev.wParam = length; m_pOutputSite->AddEvents(&ev, 1); if (m_isEdgeVoice && boundaryType == SPEI_WORD_BOUNDARY) { auto size = m_bookmarks.size(); while (m_bookmarkIndex < size) { auto& bookmark = m_bookmarks[m_bookmarkIndex]; if (offset + length <= bookmark.ulSAPITextOffset) break; OnBookmark(audioOffsetTicks, bookmark.name); m_bookmarkIndex++; } } }
void CTTSEngine::OnViseme(uint64_t offsetTicks, uint32_t visemeId) { std::lock_guard lock(m_outputSiteMutex); if (!m_pOutputSite) return; SPEVENT ev = { 0 }; ev.ullAudioStreamOffset = WaveTicksToBytes(offsetTicks); ev.eEventId = SPEI_VISEME; ev.elParamType = SPET_LPARAM_IS_UNDEFINED; ev.wParam = 0; ev.lParam = MAKELONG(visemeId, 0); m_pOutputSite->AddEvents(&ev, 1); }

void CTTSEngine::SetupSynthesizerEvents(ULONGLONG interests) { ClearSynthesizerEvents(); m_synthesizer->SynthesisStarted += [this](const SpeechSynthesisEventArgs&) { m_synthesizerStarted.store(true, std::memory_order_relaxed); }; m_synthesizerStarted.store(false, std::memory_order_relaxed); if (interests & SVEBookmark) m_synthesizer->BookmarkReached += [this](const SpeechSynthesisBookmarkEventArgs& arg) { OnBookmark(arg.AudioOffset, UTF8ToWString(arg.Text)); }; if (interests & (SVEWordBoundary | SVESentenceBoundary)) m_synthesizer->WordBoundary += [this](const SpeechSynthesisWordBoundaryEventArgs& arg) { if (arg.BoundaryType == SpeechSynthesisBoundaryType::Punctuation) return; OnBoundary(arg.AudioOffset, arg.TextOffset, arg.WordLength, arg.BoundaryType == SpeechSynthesisBoundaryType::Sentence ? SPEI_SENTENCE_BOUNDARY : SPEI_WORD_BOUNDARY); }; if (interests & SVEViseme) m_synthesizer->VisemeReceived += [this](const SpeechSynthesisVisemeEventArgs& arg) { OnViseme(arg.AudioOffset, arg.VisemeId); }; }
void CTTSEngine::ClearSynthesizerEvents() { m_synthesizer->BookmarkReached.DisconnectAll(); m_synthesizer->WordBoundary.DisconnectAll(); m_synthesizer->VisemeReceived.DisconnectAll(); m_synthesizer->SynthesisStarted.DisconnectAll(); m_synthesizer->SynthesisCompleted.DisconnectAll(); m_synthesizer->SynthesisCanceled.DisconnectAll(); }
void CTTSEngine::SetupRestAPIEvents(ULONGLONG interests) { m_restApi->AudioReceivedCallback = std::bind_front(&CTTSEngine::OnAudioData, this); if (interests & SVEBookmark) m_restApi->BookmarkCallback = [this](auto a, auto b) { OnBookmark(a, UTF8ToWString(b)); }; if ((interests & SVEWordBoundary) || (m_isEdgeVoice && (interests & SVEBookmark))) m_restApi->WordBoundaryCallback = [this](auto a, auto b, auto c) { OnBoundary(a, b, c, SPEI_WORD_BOUNDARY); }; if (interests & SVESentenceBoundary) m_restApi->SentenceBoundaryCallback = [this](auto a, auto b, auto c) { OnBoundary(a, b, c, SPEI_SENTENCE_BOUNDARY); }; if (interests & SVEViseme) m_restApi->VisemeCallback = std::bind_front(&CTTSEngine::OnViseme, this); }

void CTTSEngine::AppendTextFragToSsml(const SPVTEXTFRAG* pTextFrag) { LPCWSTR pEnd = pTextFrag->pTextStart + pTextFrag->ulTextLen; m_ssml.reserve(m_ssml.size() + pTextFrag->ulTextLen); for (LPCWSTR pCh = pTextFrag->pTextStart; pCh != pEnd && *pCh; pCh++) { switch (*pCh) { case '<': m_ssml.append(L"&lt;"); break; case '>': m_ssml.append(L"&gt;"); break; case '&': m_ssml.append(L"&amp;"); break; case '"': m_ssml.append(L"&quot;"); break; case '\'': m_ssml.append(L"&apos;"); break; default: m_ssml.push_back(*pCh); continue; } m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset + (ULONG)(pCh - pTextFrag->pTextStart) + 1, (ULONG)m_ssml.size()); } }
void CTTSEngine::AppendPhonemesToSsml(const SPPHONEID* pPhoneIds) { WCHAR phoneme[SP_MAX_PRON_LENGTH * 8]; HRESULT hr = m_phoneConverter->IdToPhone(pPhoneIds, phoneme); if (FAILED(hr)) return; for (LPCWSTR pCh = phoneme; *pCh; pCh++) { switch (*pCh) { case '<': m_ssml.append(L"&lt;"); break; case '>': m_ssml.append(L"&gt;"); break; case '&': m_ssml.append(L"&amp;"); break; case '"': m_ssml.append(L"&quot;"); break; case '\'': m_ssml.append(L"&apos;"); break; default: m_ssml.push_back(*pCh); break; } } }
void CTTSEngine::AppendSAPIContextToSsml(const SPVCONTEXT& context) { m_ssml.append(L"<say-as interpret-as='"); std::wstring_view cat = context.pCategory; if (EqualsIgnoreCase(cat.substr(0, 4), L"date") && (cat[4] == '_' || cat[4] == ':')) { m_ssml.append(L"date' format='"); auto fmt = cat.substr(5); if (EqualsIgnoreCase(fmt, L"year")) m_ssml.push_back('y'); else m_ssml.append(fmt); } else if (EqualsIgnoreCase(cat, L"number_cardinal")) m_ssml.append(L"cardinal"); else if (EqualsIgnoreCase(cat, L"number_fraction")) m_ssml.append(L"fraction"); else if (EqualsIgnoreCase(cat, L"phone_number")) m_ssml.append(L"telephone"); else m_ssml.append(cat); m_ssml.append(L"'>"); }

static bool NeedAddingSpace(std::wstring_view ssmlBefore, std::wstring_view strAfter) { if (strAfter.empty()) return false; wchar_t chBefore = ssmlBefore.back(); if (chBefore == L'>') return false; if (iswspace(chBefore)) return false; if (chBefore < 128) return true; wchar_t chAfter = strAfter.front(); if (iswspace(chAfter)) return false; if (chAfter < 128) return true; WORD wTypeBefore = 0, wTypeAfter = 0; GetStringTypeW(CT_CTYPE3, &chBefore, 1, &wTypeBefore); GetStringTypeW(CT_CTYPE3, &chAfter, 1, &wTypeAfter); if ((wTypeBefore & wTypeAfter) & (C3_IDEOGRAPH | C3_KATAKANA | C3_HIRAGANA)) return false; return true; }
static std::wstring_view GetXMLTagName(const std::wstring& tag) { auto begin = std::find_if_not(tag.begin() + 1, tag.end() - 1, iswspace); auto end = std::find_if(begin, tag.end() - 1, iswspace); return std::wstring_view(begin, end); }
static std::wstring_view GetXMLClosingTagName(std::wstring_view tag) { auto slash = std::find(tag.begin() + 1, tag.end() - 1, L'/'); auto begin = std::find_if_not(slash + 1, tag.end() - 1, iswspace); auto end = std::find_if(begin, tag.end() - 1, iswspace); return std::wstring_view(begin, end); }
static bool IsXMLClosingTag(std::wstring_view tag) { if (tag.size() < 4) return false; auto it = std::find_if_not(tag.begin() + 1, tag.end() - 1, iswspace); return it != tag.end() - 1 && *it == L'/'; }
static bool IsXMLSelfClosingTag(std::wstring_view tag) { if (tag.size() < 4) return false; auto it = std::find_if_not(tag.rbegin() + 1, tag.rend() - 1, iswspace); return it != tag.rend() - 1 && *it == L'/'; }

bool CTTSEngine::BuildSSML(const SPVTEXTFRAG* pTextFragList)
{
    m_ssml.assign(L"<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='http://www.w3.org/2001/mstts' xml:lang='");
    m_ssml.append(m_localeName);
    m_ssml.append(L"'>");

    USHORT mainVolume; if (FAILED(m_pOutputSite->GetVolume(&mainVolume))) mainVolume = 100;
    long mainRate; if (FAILED(m_pOutputSite->GetRate(&mainRate))) mainRate = 0;

    bool isInProsodyTag = false, isInEmphasisTag = false, isInSayAsTag = false;
    std::vector<std::wstring> customTags; bool isInCustomTags = false;
    bool isEdgeVoice = m_isEdgeVoice;
    
    // 【核心修复】判断是否为本地离线语音
    bool isLocalVoice = (m_synthesizer != nullptr); 

    int prosodyCount = 0;
    bool hasText = false;
    m_offsetMappings.clear(); m_mappingIndex = 0;
    m_bookmarks.clear(); m_bookmarkIndex = 0;
    ULONG lastSAPIOffset = 0;

    if (!m_onlineVoiceName.empty()) { m_ssml.append(L"<voice name='"); m_ssml.append(m_onlineVoiceName); m_ssml.append(L"'>"); }

    constexpr auto IsSpeakableEdgeFrag = [](const SPVTEXTFRAG* pTextFrag) { auto action = pTextFrag->State.eAction; return action == SPVA_Speak || action == SPVA_SpellOut || action == SPVA_Pronounce; };

    for (auto pTextFrag = pTextFragList; pTextFrag; pTextFrag = pTextFrag->pNext)
    {
        if (pTextFrag->State.eAction != SPVA_Bookmark && pTextFrag->ulTextLen != 0) hasText = true;

        if (!isInProsodyTag && (!isEdgeVoice || IsSpeakableEdgeFrag(pTextFrag)))
        {
            USHORT volume = (USHORT)std::clamp(mainVolume * pTextFrag->State.Volume / 100, 0UL, 100UL);
            long rate = std::clamp(mainRate + pTextFrag->State.RateAdj, -10L, 10L);
            long pitch = std::clamp(pTextFrag->State.PitchAdj.MiddleAdj, -10L, 10L);

            if (volume != 100 || rate != 0 || pitch != 0) 
            {
                prosodyCount++;
                m_ssml.append(L"<prosody");
                if (volume != 100) { m_ssml.append(L" volume='"); m_ssml.append(std::to_wstring(volume - 100)); m_ssml.append(L"%'"); }
                if (rate != 0) { m_ssml.append(L" rate='"); m_ssml.append(std::to_wstring(rate >= 0 ? rate * 20 : rate * 20 / 3)); m_ssml.append(L"%'"); }
                if (pitch != 0) { m_ssml.append(L" pitch='"); m_ssml.append(std::to_wstring(pitch * 5)); m_ssml.append(L"%'"); }
                m_ssml.push_back('>');
                isInProsodyTag = true;
            }
        }

        // 本地语音不支持 emphasis 标签
        if (!isInEmphasisTag && pTextFrag->State.EmphAdj && !isEdgeVoice && !isLocalVoice) { m_ssml.append(L"<emphasis>"); isInEmphasisTag = true; }

        if (!isInCustomTags) { for (const auto& customTag : customTags) m_ssml.append(customTag); isInCustomTags = true; }

        // 本地语音不支持 say-as 标签
        if (!isInSayAsTag && pTextFrag->State.Context.pCategory && pTextFrag->State.eAction == SPVA_Speak && !isEdgeVoice && !isLocalVoice) { AppendSAPIContextToSsml(pTextFrag->State.Context); isInSayAsTag = true; }

        if (isEdgeVoice)
        {
            switch (pTextFrag->State.eAction)
            {
            case SPVA_Speak: case SPVA_SpellOut: case SPVA_Pronounce:
                if (NeedAddingSpace(m_ssml, std::wstring_view(pTextFrag->pTextStart, pTextFrag->ulTextLen))) m_ssml.push_back(L' ');
                m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset, (ULONG)m_ssml.size()); AppendTextFragToSsml(pTextFrag); m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset + pTextFrag->ulTextLen, (ULONG)m_ssml.size()); lastSAPIOffset = pTextFrag->ulTextSrcOffset + pTextFrag->ulTextLen; break;
            case SPVA_Bookmark:
                m_offsetMappings.emplace_back(lastSAPIOffset, (ULONG)m_ssml.size()); m_bookmarks.emplace_back(pTextFrag->ulTextSrcOffset, std::wstring(pTextFrag->pTextStart, pTextFrag->ulTextLen)); break;
            }
        }
        else
        {
            switch (pTextFrag->State.eAction)
            {
            case SPVA_Speak:
                if (NeedAddingSpace(m_ssml, std::wstring_view(pTextFrag->pTextStart, pTextFrag->ulTextLen))) m_ssml.push_back(L' ');
                m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset, (ULONG)m_ssml.size()); AppendTextFragToSsml(pTextFrag); m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset + pTextFrag->ulTextLen, (ULONG)m_ssml.size()); break;

            case SPVA_Silence: 
                if (!isLocalVoice) { m_ssml.append(L"<break time='"); m_ssml.append(std::to_wstring(pTextFrag->State.SilenceMSecs)); m_ssml.append(L"ms'/>"); } 
                break;

            case SPVA_Bookmark: 
                m_ssml.append(L"<bookmark mark='"); AppendTextFragToSsml(pTextFrag); m_ssml.append(L"'/>"); m_bookmarks.emplace_back(pTextFrag->ulTextSrcOffset, std::wstring(pTextFrag->pTextStart, pTextFrag->ulTextLen)); break;

            case SPVA_SpellOut: 
                if (isLocalVoice) {
                    // 本地语音不支持 spell-out，直接读字符
                    if (NeedAddingSpace(m_ssml, std::wstring_view(pTextFrag->pTextStart, pTextFrag->ulTextLen))) m_ssml.push_back(L' ');
                    m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset, (ULONG)m_ssml.size()); AppendTextFragToSsml(pTextFrag); m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset + pTextFrag->ulTextLen, (ULONG)m_ssml.size());
                } else {
                    m_ssml.append(L"<say-as interpret-as='characters'>"); m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset, (ULONG)m_ssml.size()); AppendTextFragToSsml(pTextFrag); m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset + pTextFrag->ulTextLen, (ULONG)m_ssml.size()); m_ssml.append(L"</say-as>");
                }
                break;

            case SPVA_Pronounce: 
                // 【核心修复】本地语音直接忽略拼音，当普通文本读，彻底避免 x86 ONNX 解析拼音越界崩溃
                if (isLocalVoice) {
                    if (NeedAddingSpace(m_ssml, std::wstring_view(pTextFrag->pTextStart, pTextFrag->ulTextLen))) m_ssml.push_back(L' ');
                    m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset, (ULONG)m_ssml.size()); AppendTextFragToSsml(pTextFrag); m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset + pTextFrag->ulTextLen, (ULONG)m_ssml.size());
                } else {
                    m_ssml.append(L"<phoneme alphabet='sapi' ph='"); AppendPhonemesToSsml(pTextFrag->State.pPhoneIds); m_ssml.append(L"'>"); m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset, (ULONG)m_ssml.size()); AppendTextFragToSsml(pTextFrag); m_offsetMappings.emplace_back(pTextFrag->ulTextSrcOffset + pTextFrag->ulTextLen, (ULONG)m_ssml.size()); m_ssml.append(L"</phoneme>");
                }
                break;

            case SPVA_ParseUnknownTag: 
            {
                if (isLocalVoice) break; // 本地语音忽略所有未知自定义标签
                std::wstring_view tag(pTextFrag->pTextStart, pTextFrag->ulTextLen);
                tag.remove_prefix(tag.find('<')); tag.remove_suffix(tag.size() - tag.rfind(L'>') - 1);
                if (IsXMLSelfClosingTag(tag)) { m_ssml.append(pTextFrag->pTextStart, pTextFrag->ulTextLen); }
                else if (IsXMLClosingTag(tag)) {
                    std::wstring_view tagName = GetXMLClosingTagName(tag);
                    auto tagToClose = std::find_if(customTags.rbegin(), customTags.rend(), [tagName](const std::wstring& tag) { return EqualsIgnoreCase(GetXMLTagName(tag), tagName); });
                    if (tagToClose != customTags.rend()) {
                        if (tagToClose != customTags.rbegin()) { for (auto it = customTags.rbegin(); it != tagToClose; ++it) { m_ssml.append(L"</"); m_ssml.append(GetXMLTagName(*it)); m_ssml.push_back(L'>'); } }
                        m_ssml.append(pTextFrag->pTextStart, pTextFrag->ulTextLen); customTags.erase(tagToClose.base() - 1, customTags.end());  
                    }
                }
                else if (pTextFrag->ulTextLen >= 3) { m_ssml.append(pTextFrag->pTextStart, pTextFrag->ulTextLen); customTags.emplace_back(pTextFrag->pTextStart, pTextFrag->ulTextLen); }
                break;
            }
            }
        }

        int preserveTagLevel = 0;
        auto pNextTextFrag = pTextFrag->pNext;
        if (isEdgeVoice) { for (; pNextTextFrag; pNextTextFrag = pNextTextFrag->pNext) { if (IsSpeakableEdgeFrag(pNextTextFrag)) break; } }

        if (pNextTextFrag)
        {
            auto& curState = pTextFrag->State; auto& nextState = pNextTextFrag->State;
            bool sameProsody = (curState.Volume == nextState.Volume && curState.RateAdj == nextState.RateAdj && curState.PitchAdj.MiddleAdj == nextState.PitchAdj.MiddleAdj);
            if (sameProsody || (isEdgeVoice && prosodyCount >= 2))
            {
                preserveTagLevel = 1; 
                if (curState.EmphAdj == nextState.EmphAdj)
                {
                    preserveTagLevel = 2; 
                    if ((curState.Context.pCategory == nextState.Context.pCategory || (curState.Context.pCategory && nextState.Context.pCategory && _wcsicmp(curState.Context.pCategory, nextState.Context.pCategory) == 0)) && nextState.eAction == SPVA_Speak) preserveTagLevel = 3;
                }
            }
        }

        if (isInSayAsTag && preserveTagLevel < 3) { m_ssml.append(L"</say-as>"); isInSayAsTag = false; }
        if (isInCustomTags && preserveTagLevel < 2) { for (auto it = customTags.rbegin(); it != customTags.rend(); ++it) { m_ssml.append(L"</"); m_ssml.append(GetXMLTagName(*it)); m_ssml.push_back(L'>'); } isInCustomTags = false; }
        if (isInEmphasisTag && preserveTagLevel < 2) { m_ssml.append(L"</emphasis>"); isInEmphasisTag = false; }
        if (isInProsodyTag && preserveTagLevel < 1) { m_ssml.append(L"</prosody>"); isInProsodyTag = false; }
    }

    if (!m_onlineVoiceName.empty()) m_ssml.append(L"</voice>");
    m_ssml.append(L"</speak>");
    return hasText;
}

void CTTSEngine::FinishSimulatingBookmarkEvents(ULONGLONG streamOffset) { const auto size = m_bookmarks.size(); SPEVENT ev = { 0 }; ev.ullAudioStreamOffset = streamOffset; ev.eEventId = SPEI_TTS_BOOKMARK; ev.elParamType = SPET_LPARAM_IS_STRING; for (auto i = m_bookmarkIndex; i < size; i++) { auto& bookmark = m_bookmarks[i]; ev.lParam = reinterpret_cast<LPARAM>(bookmark.name.c_str()); ev.wParam = _wtol(bookmark.name.c_str()); m_pOutputSite->AddEvents(&ev, 1); } }
void CTTSEngine::MapTextOffset(ULONG& ulSSMLOffset, ULONG& ulTextLen) { if (m_offsetMappings.empty()) return; ULONG endOffset = ulSSMLOffset + ulTextLen; const auto size = m_offsetMappings.size(); if (m_mappingIndex >= size || m_offsetMappings[m_mappingIndex].ulSSMLTextOffset > ulSSMLOffset) m_mappingIndex = 0; while (m_mappingIndex + 1 < size && ulSSMLOffset >= m_offsetMappings[m_mappingIndex + 1].ulSSMLTextOffset) m_mappingIndex++; const auto& mapping = m_offsetMappings[m_mappingIndex]; auto& ulSAPIOffset = ulSSMLOffset; if (mapping.ulSSMLTextOffset > mapping.ulSAPITextOffset && ulSSMLOffset < mapping.ulSSMLTextOffset - mapping.ulSAPITextOffset) ulSAPIOffset = 0; else ulSAPIOffset = ulSSMLOffset - mapping.ulSSMLTextOffset + mapping.ulSAPITextOffset; auto index = m_mappingIndex; while (index + 1 < size && endOffset >= m_offsetMappings[index + 1].ulSSMLTextOffset) index++; auto endSAPIOffset = ulSAPIOffset + ulTextLen; while (index > 0 && m_offsetMappings[index - 1].ulSSMLTextOffset == m_offsetMappings[index].ulSSMLTextOffset && m_offsetMappings[index - 1].ulSAPITextOffset >= endSAPIOffset) index--; if (index <= m_mappingIndex) return; const auto& endMapping = m_offsetMappings[index]; endOffset = endOffset - endMapping.ulSSMLTextOffset + endMapping.ulSAPITextOffset; ulTextLen = endOffset - ulSSMLOffset; }
void CTTSEngine::CheckSynthesisResult(const std::shared_ptr<SpeechSynthesisResult>& result) { if (result->Reason != ResultReason::Canceled) return; auto details = SpeechSynthesisCancellationDetails::FromResult(result); if (details->Reason != CancellationReason::Error) return; throw std::runtime_error(UTF8ToAnsi(details->ErrorDetails)); }
