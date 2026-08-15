STDMETHODIMP CTTSEngine::Speak(DWORD /*dwSpeakFlags*/,
    REFGUID /*rguidFormatId*/,
    const WAVEFORMATEX* /*pWaveFormatEx*/,
    const SPVTEXTFRAG* pTextFragList,
    ISpTTSEngineSite* pOutputSite) noexcept
{
    ScopeTracer tracer("Speak: begin", "Speak: end");
    try
    {
        // Check args
        if (SP_IS_BAD_INTERFACE_PTR(pOutputSite) ||
            SP_IS_BAD_READ_PTR(pTextFragList))
        {
            return E_INVALIDARG;
        }
        if (!m_synthesizer && !m_restApi)
        {
            return SPERR_UNINITIALIZED;
        }

        // 【修复 1】获取实例锁，确保同一个引擎实例的朗读和取消严格串行，防止 SDK 状态机错乱
        std::unique_lock<std::mutex> lock(m_speakMutex);

        if (m_lastCancellingFuture.valid())
        {
            // The previous cancellation is still in progress. Wait for it.
            while (m_lastCancellingFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout)
            {
                if (pOutputSite->GetActions() & SPVES_ABORT)
                {
                    return S_OK;
                }
                lock.unlock(); // 短暂释放锁避免死锁
                Sleep(0);  
                lock.lock();
            }
            m_lastCancellingFuture = {};
        }

        ULONGLONG eventInterests = 0;
        pOutputSite->GetEventInterest(&eventInterests);
        if (m_synthesizer)
            SetupSynthesizerEvents(eventInterests);
        else
            SetupRestAPIEvents(eventInterests);

        // Clear m_pOutputSite automatically when Speak is completed
        ScopeGuard siteDeleter([this]()
            {
                std::lock_guard lock(m_outputSiteMutex);
                m_pOutputSite = nullptr;
            });
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
        m_onlineDelayOptimization =
            !m_onlineVoiceName.empty() && RegOpenConfigKey().GetDword(L"EnableOnlineDelayOptimization");

        std::future<void> future;

        if (m_synthesizer)
        {
            // 【修复 2】核心修复：将 m_ssml 拷贝一份传入后台线程！
            // 防止主线程在取消或下一次朗读时覆盖 m_ssml，导致后台 ONNX 引擎读取到脏数据而崩溃。
            std::wstring ssmlCopy = m_ssml;
            future = std::async(std::launch::async, [this, ssml = std::move(ssmlCopy)]() { 
                CheckSynthesisResult(m_synthesizer->SpeakSsml(ssml)); 
            });
        }
        else
        {
            future = m_restApi->SpeakAsync(m_ssml);
        }

        while (!(pOutputSite->GetActions() & SPVES_ABORT)
            && future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout)
        {
            if (pOutputSite->GetActions() & SPVES_SKIP)
            {
                LogWarn("Speak: Skipping not supported, ignored");
                pOutputSite->CompleteSkip(0);
            }
            lock.unlock(); // 等待期间释放锁，让事件回调线程能够正常运行
            Sleep(10);
            lock.lock();
        }

        if (pOutputSite->GetActions() & SPVES_ABORT) // requested stop
        {
            LogDebug("Speak: Requested stop");
            if (m_synthesizer)
            {
                m_lastCancellingFuture = std::async(
                    std::launch::async, [this, future = std::move(future)]() mutable
                {
                    while (!m_synthesizerStarted.load(std::memory_order_relaxed))
                        Sleep(0);
                    m_synthesizer->StopSpeakingAsync().wait();
                    future.wait();
                });
            }
            else
                m_restApi->Stop();

            m_lastSpeakCompletedTicks = 0;
            // 锁会在函数返回时自动释放
        }
        else
        {
            lock.unlock(); // 等待合成完成期间释放锁
            future.get();  // wait for the future and get its stored exception thrown
            lock.lock();
            
            if (m_isEdgeVoice)
            {
                FinishSimulatingBookmarkEvents(
                    m_restApi->GetWaveBytesWritten() + m_compensatedSilentBytes - m_lastSilentBytes);
            }

            m_lastSpeakCompletedTicks = _GetTickCount();
        }

        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        LogCritical("Out of memory");
        return E_OUTOFMEMORY;
    }
    catch (const std::system_error& ex)
    {
        return OnException(ex, "Speak: {}");
    }
    catch (const std::exception& ex)
    {
        return OnException(ex, "Speak: {}");
    }
    catch (...) 
    {
        LogErr("Speak: Unknown error");
        return E_FAIL;
    }
}
