#include "pch.h"
#include "MagService.h"
#include "HotkeyService.h"
#include "Win32Utils.h"


namespace winrt::Magpie::App {

MagService::MagService() {
	App app = Application::Current().as<App>();
	_settings = app.Settings();
	_magRuntime = app.MagRuntime();

	_dispatcher = CoreWindow::GetForCurrentThread().Dispatcher();

	_timer.Interval(TimeSpan(std::chrono::milliseconds(25)));
	_timerTickRevoker = _timer.Tick(
		auto_revoke,
		{ this, &MagService::_Timer_Tick }
	);

	_settings.IsAutoRestoreChanged({ this, &MagService::_Settings_IsAutoRestoreChanged });
	_magRuntime.IsRunningChanged({ this, &MagService::_MagRuntime_IsRunningChanged });

	HotkeyService::Get().HotkeyPressed(
		{ this, &MagService::_HotkeyService_HotkeyPressed }
	);

	_UpdateIsAutoRestore();

	_hwndHost = (HWND)Application::Current().as<Magpie::App::App>().HwndHost();
}

void MagService::StartCountdown() {
	if (_tickingDownCount != 0) {
		return;
	}

	_tickingDownCount = _settings.DownCount();
	_timerStartTimePoint = std::chrono::steady_clock::now();
	_timer.Start();
	_isCountingDownChangedEvent(true);
}

void MagService::StopCountdown() {
	if (_tickingDownCount == 0) {
		return;
	}

	_tickingDownCount = 0;
	_timer.Stop();
	_isCountingDownChangedEvent(false);
}

float MagService::CountdownLeft() const noexcept {
	using namespace std::chrono;

	if (!IsCountingDown()) {
		return 0.0f;
	}

	// DispatcherTimer 误差很大，因此我们自己计算剩余时间
	auto now = steady_clock::now();
	int timeLeft = (int)duration_cast<milliseconds>(_timerStartTimePoint + seconds(_tickingDownCount) - now).count();
	return timeLeft / 1000.0f;
}

void MagService::ClearWndToRestore() {
	if (_wndToRestore == 0) {
		return;
	}

	_wndToRestore = 0;
	_wndToRestoreChangedEvent(_wndToRestore);
}

void MagService::_HotkeyService_HotkeyPressed(HotkeyAction action) {
	switch (action) {
	case HotkeyAction::Scale:
	{
		if (_magRuntime.IsRunning()) {
			_magRuntime.Stop();
			return;
		}

		_StartScale();
		break;
	}
	case HotkeyAction::Overlay:
	{
		if (_magRuntime.IsRunning()) {
			_magRuntime.ToggleOverlay();
			return;
		}
		break;
	}
	default:
		break;
	}
}

void MagService::_Timer_Tick(IInspectable const&, IInspectable const&) {
	float timeLeft = CountdownLeft();

	// 剩余时间在 10 ms 以内计时结束
	if (timeLeft < 0.01) {
		StopCountdown();
		_StartScale();
		return;
	}

	_countdownTickEvent(timeLeft);
}

void MagService::_Settings_IsAutoRestoreChanged(IInspectable const&, bool) {
	_UpdateIsAutoRestore();
}

IAsyncAction MagService::_MagRuntime_IsRunningChanged(IInspectable const&, bool) {
	co_await _dispatcher.RunAsync(CoreDispatcherPriority::Normal, [this]() {
		if (_magRuntime.IsRunning()) {
			StopCountdown();

			if (_settings.IsAutoRestore()) {
				_curSrcWnd = (HWND)_magRuntime.HwndSrc();
				_wndToRestore = 0;
				_wndToRestoreChangedEvent(_wndToRestore);
			}
		} else {
			// 必须在主线程还原主窗口样式
			// 见 FrameSourceBase::~FrameSourceBase
			LONG_PTR style = GetWindowLongPtr(_hwndHost, GWL_STYLE);
			if (!(style & WS_THICKFRAME)) {
				SetWindowLongPtr(_hwndHost, GWL_STYLE, style | WS_THICKFRAME);
				SetWindowPos(_hwndHost, 0, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
			}

			if (_settings.IsAutoRestore()) {
				// 退出全屏之后前台窗口不变则不必记忆
				if (IsWindow(_curSrcWnd) && GetForegroundWindow() != _curSrcWnd) {
					_wndToRestore = (uint64_t)_curSrcWnd;
					_wndToRestoreChangedEvent(_wndToRestore);
				}

				_curSrcWnd = NULL;
			}
		}
	});
}

void MagService::_UpdateIsAutoRestore() {
	if (_settings.IsAutoRestore()) {
		// 立即生效，即使正处于缩放状态
		_curSrcWnd = (HWND)_magRuntime.HwndSrc();

		// 监听前台窗口更改
		_hForegroundEventHook = SetWinEventHook(
			EVENT_SYSTEM_FOREGROUND,
			EVENT_SYSTEM_FOREGROUND,
			NULL,
			_WinEventProcCallback,
			0,
			0,
			WINEVENT_OUTOFCONTEXT
		);
		// 监听窗口销毁
		_hDestoryEventHook = SetWinEventHook(
			EVENT_OBJECT_DESTROY,
			EVENT_OBJECT_DESTROY,
			NULL,
			_WinEventProcCallback,
			0,
			0,
			WINEVENT_OUTOFCONTEXT
		);
	} else {
		_curSrcWnd = NULL;
		_wndToRestore = 0;
		_wndToRestoreChangedEvent(_wndToRestore);
		if (_hForegroundEventHook) {
			UnhookWinEvent(_hForegroundEventHook);
			_hForegroundEventHook = NULL;
		}
		if (_hDestoryEventHook) {
			UnhookWinEvent(_hDestoryEventHook);
			_hDestoryEventHook = NULL;
		}
	}
}

void MagService::_CheckForeground() {
	if (_wndToRestore == 0 || _magRuntime.IsRunning()) {
		return;
	}

	if (!IsWindow((HWND)_wndToRestore)) {
		_wndToRestore = 0;
		_wndToRestoreChangedEvent(_wndToRestore);
		return;
	}

	if ((HWND)_wndToRestore != GetForegroundWindow()) {
		return;
	}

	_StartScale(_wndToRestore);
}

void MagService::_StartScale(uint64_t hWnd) {
	if (hWnd == 0) {
		hWnd = (uint64_t)GetForegroundWindow();
	}

	if (Win32Utils::GetWindowShowCmd((HWND)hWnd) != SW_NORMAL) {
		return;
	}

	Magpie::Runtime::MagSettings magSettings;
	magSettings.CopyFrom(_settings.GetMagSettings(_wndToRestore));

	// 应用全局配置
	magSettings.IsBreakpointMode(_settings.IsBreakpointMode());
	magSettings.IsDisableEffectCache(_settings.IsDisableEffectCache());
	magSettings.IsSaveEffectSources(_settings.IsSaveEffectSources());
	magSettings.IsWarningsAreErrors(_settings.IsWarningsAreErrors());
	magSettings.IsSimulateExclusiveFullscreen(_settings.IsSimulateExclusiveFullscreen());

	_magRuntime.Run(hWnd, magSettings);
}

void MagService::_WinEventProcCallback(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
	MagService::Get()._CheckForeground();
}

}