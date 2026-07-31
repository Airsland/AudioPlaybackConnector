#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
void ShowDevicePicker();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_ACTIVATE:
		// The helper window hosts XAML flyouts; whenever it loses activation
		// (user clicks elsewhere, the shell's tray overflow popup closes,
		// etc.) dismiss any open popup and hide the window so it can never
		// remain floating on screen.
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			if (g_xamlMenu)
				g_xamlMenu.Hide();
			if (g_xamlFlyout)
				g_xamlFlyout.Hide();
			ShowWindow(g_hWnd, SW_HIDE);
		}
		break;
	case WM_DESTROY:
		g_volumeBoostMgr.Stop();
		{
			// Move the map out first: closing a connection can fire
			// StateChanged(Closed) synchronously, which erases from the map
			// and would invalidate the iteration below.
			auto connections = std::move(g_audioPlaybackConnections);
			g_audioPlaybackConnections.clear();
			for (const auto& connection : connections)
			{
				connection.second.second.Close();
				g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
			}
		}
		SaveSettings();
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
			ShowDevicePicker();
			break;
		case WM_CONTEXTMENU:
		{
			auto dpi = GetDpiForWindow(hWnd);
			POINT pt;
			pt.x = GET_X_LPARAM(wParam);
			pt.y = GET_Y_LPARAM(wParam);
			if (pt.x == 0 && pt.y == 0)
				GetCursorPos(&pt);
			Point point = {
				static_cast<float>(pt.x * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(pt.y * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			// Reset the helper window back to the origin before showing the
			// menu, otherwise a previous exit-confirm flyout may have moved
			// it next to the tray icon and the menu would be offset.
			SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW | SWP_NOSIZE);
			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutShowOptions options;
			options.Position(point);
			g_xamlMenu.ShowAt(g_xamlCanvas, options);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(g_devicePicker, i);
			}
			g_lastDevices.clear();
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);
	flyout.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlFlyout = flyout;
}

// The slider covers the endpoint's full -96..+30 dB range, but the position
// is not linear in dB. dB is already a logarithmic (perceptual) scale -
// +10 dB is roughly "twice as loud" - and Windows applies a further taper to
// its own volume sliders so that the region around 0 dB (the normal 100%
// level) and above gets most of the slider travel. We replicate that curve
// (position = normalizedDb^2.29), which matches the mapping Windows itself
// uses for IAudioEndpointVolume.
static constexpr float kBoostRangeMin = -96.0f;
static constexpr float kBoostRangeMax = 30.0f;
static constexpr float kBoostSliderMax = 1000.0f;
static constexpr float kBoostTaper = 2.29f;

static float BoostSliderToDb(float position)
{
	float p = position / kBoostSliderMax;
	if (p < 0.0f)
		p = 0.0f;
	else if (p > 1.0f)
		p = 1.0f;
	float n = std::pow(p, 1.0f / kBoostTaper);
	return kBoostRangeMin + n * (kBoostRangeMax - kBoostRangeMin);
}

static float BoostDbToSlider(float db)
{
	float n = (db - kBoostRangeMin) / (kBoostRangeMax - kBoostRangeMin);
	if (n < 0.0f)
		n = 0.0f;
	else if (n > 1.0f)
		n = 1.0f;
	return std::pow(n, kBoostTaper) * kBoostSliderMax;
}

static bool IsLightTheme()
{
	DWORD value = 0, cbValue = sizeof(value);
	RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue);
	return value != 0;
}

static winrt::Windows::UI::Color Argb(BYTE a, BYTE r, BYTE g, BYTE b)
{
	winrt::Windows::UI::Color c = {};
	c.A = a;
	c.R = r;
	c.G = g;
	c.B = b;
	return c;
}

// A clickable menu row (icon + label) with hover feedback.
static Button MakeMenuRow(const wchar_t* text, const wchar_t* glyph, SolidColorBrush hoverBrush, RoutedEventHandler handler)
{
	FontIcon icon;
	icon.Glyph(glyph);
	icon.FontSize(14);
	icon.VerticalAlignment(VerticalAlignment::Center);

	TextBlock label;
	label.Text(_(text));
	label.Margin({ 8, 0, 0, 0 });
	label.VerticalAlignment(VerticalAlignment::Center);

	StackPanel panel;
	panel.Orientation(Orientation::Horizontal);
	panel.Children().Append(icon);
	panel.Children().Append(label);

	Button button;
	button.Content(panel);
	button.Background(nullptr);
	button.BorderThickness({ 0, 0, 0, 0 });
	button.HorizontalAlignment(HorizontalAlignment::Stretch);
	button.HorizontalContentAlignment(HorizontalAlignment::Left);
	button.Padding({ 10, 7, 10, 7 });
	button.CornerRadius({ 4, 4, 4, 4 });
	button.Click(handler);
	button.PointerEntered([hoverBrush](const auto& sender, const auto&) {
		sender.as<Button>().Background(hoverBrush);
	});
	button.PointerExited([](const auto& sender, const auto&) {
		sender.as<Button>().Background(nullptr);
	});
	return button;
}

void ShowDevicePicker()
{
	using namespace winrt::Windows::UI::Popups;

	RECT iconRect;
	auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
	if (FAILED(hr))
	{
		LOG_HR(hr);
		return;
	}

	auto dpi = GetDpiForWindow(g_hWnd);
	Rect rect = {
		static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
		static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
		static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
		static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
	};

	SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
	SetForegroundWindow(g_hWnd);
	g_devicePicker.Show(rect, Placement::Above);
}

void SetupMenu()
{
	const bool light = IsLightTheme();

	SolidColorBrush bgBrush;
	bgBrush.Color(Argb(255, light ? 247 : 32, light ? 247 : 32, light ? 249 : 32));
	SolidColorBrush borderBrush;
	borderBrush.Color(Argb(255, light ? 232 : 60, light ? 232 : 60, light ? 232 : 60));
	SolidColorBrush hoverBrush;
	hoverBrush.Color(Argb(255, light ? 224 : 48, light ? 224 : 48, light ? 224 : 48));
	SolidColorBrush textBrush;
	textBrush.Color(Argb(255, light ? 27 : 243, light ? 27 : 243, light ? 27 : 243));
	SolidColorBrush subtextBrush;
	subtextBrush.Color(Argb(255, light ? 96 : 170, light ? 96 : 170, light ? 96 : 170));

	TextBlock title;
	title.Text(_(L"AudioPlaybackConnector"));
	title.Foreground(textBrush);
	title.Margin({ 10, 4, 10, 6 });

	// Volume header: caption on the left, current level on the right.
	Grid volumeHeader;
	volumeHeader.Margin({ 10, 8, 10, 0 });
	ColumnDefinition columnCaption;
	columnCaption.Width(GridLength{ 1, GridUnitType::Star });
	ColumnDefinition columnValue;
	columnValue.Width(GridLength{ 1, GridUnitType::Auto });
	volumeHeader.ColumnDefinitions().Append(columnCaption);
	volumeHeader.ColumnDefinitions().Append(columnValue);

	TextBlock volumeCaption;
	volumeCaption.Text(_(L"Phone volume"));
	volumeCaption.Foreground(textBrush);
	volumeCaption.VerticalAlignment(VerticalAlignment::Center);

	TextBlock volumeValue;
	volumeValue.Foreground(subtextBrush);
	volumeValue.VerticalAlignment(VerticalAlignment::Center);

	Grid::SetColumn(volumeValue, 1);
	volumeHeader.Children().Append(volumeCaption);
	volumeHeader.Children().Append(volumeValue);

	Slider slider;
	slider.Minimum(0);
	slider.Maximum(kBoostSliderMax);
	slider.Value(BoostDbToSlider(g_volumeBoostDb));
	slider.Margin({ 10, 4, 10, 0 });
	slider.ValueChanged([volumeValue](const auto&, const auto& args) {
		g_volumeBoostDb = BoostSliderToDb(static_cast<float>(args.NewValue()));
		if (std::fabs(g_volumeBoostDb) < 0.05f)
		{
			g_volumeBoostDb = 0.0f;
			g_volumeBoostMgr.Stop();
		}
		else if (g_volumeBoostMgr.IsActive())
			g_volumeBoostMgr.SetGain(g_volumeBoostDb);
		else
			g_volumeBoostMgr.Start(g_volumeBoostDb, g_lastConnectedDeviceName);

		wchar_t buf[32] = {};
		swprintf(buf, 32, L"%+.1f dB", static_cast<double>(g_volumeBoostDb));
		volumeValue.Text(buf);
	});

	Button resetButton;
	resetButton.Content(winrt::box_value(_(L"Reset to 0 dB")));
	resetButton.Background(nullptr);
	resetButton.BorderThickness({ 0, 0, 0, 0 });
	resetButton.HorizontalAlignment(HorizontalAlignment::Left);
	resetButton.Padding({ 10, 4, 10, 4 });
	resetButton.Margin({ 10, 0, 10, 0 });
	resetButton.CornerRadius({ 4, 4, 4, 4 });
	resetButton.Click([slider, volumeValue](const auto&, const auto&) {
		slider.Value(BoostDbToSlider(0.0f));
		g_volumeBoostDb = 0.0f;
		g_volumeBoostMgr.Stop();
		volumeValue.Text(L"+0.0 dB");
	});
	resetButton.PointerEntered([hoverBrush](const auto& sender, const auto&) {
		sender.as<Button>().Background(hoverBrush);
	});
	resetButton.PointerExited([](const auto& sender, const auto&) {
		sender.as<Button>().Background(nullptr);
	});

	auto connectButton = MakeMenuRow(_(L"Connect device"), L"\xE720", hoverBrush, [](const auto&, const auto&) {
		if (g_xamlMenu)
			g_xamlMenu.Hide();
		ShowDevicePicker();
	});

	auto settingsButton = MakeMenuRow(_(L"Bluetooth settings"), L"\xE713", hoverBrush, [](const auto&, const auto&) {
		if (g_xamlMenu)
			g_xamlMenu.Hide();
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	ToggleSwitch reconnectToggle;
	reconnectToggle.Header(winrt::box_value(_(L"Reconnect on start")));
	reconnectToggle.IsOn(g_reconnect);
	reconnectToggle.Margin({ 4, 4, 4, 4 });
	reconnectToggle.Toggled([](const auto& sender, const auto&) {
		g_reconnect = sender.as<ToggleSwitch>().IsOn();
	});

	auto exitButton = MakeMenuRow(_(L"Exit"), L"\xE8BB", hoverBrush, [](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	auto makeSeparator = [borderBrush]() {
		Border separator;
		separator.Height(1);
		separator.Background(borderBrush);
		separator.Margin({ 10, 6, 10, 6 });
		return separator;
	};

	StackPanel panel;
	panel.Spacing(2);
	panel.Children().Append(title);
	panel.Children().Append(makeSeparator());
	panel.Children().Append(connectButton);
	panel.Children().Append(settingsButton);
	panel.Children().Append(makeSeparator());
	panel.Children().Append(volumeHeader);
	panel.Children().Append(slider);
	panel.Children().Append(resetButton);
	panel.Children().Append(makeSeparator());
	panel.Children().Append(reconnectToggle);
	panel.Children().Append(makeSeparator());
	panel.Children().Append(exitButton);

	Border border;
	border.CornerRadius({ 8, 8, 8, 8 });
	border.Background(bgBrush);
	border.BorderBrush(borderBrush);
	border.BorderThickness({ 1, 1, 1, 1 });
	border.Padding({ 6, 4, 6, 6 });
	border.Width(270);
	border.Child(panel);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(border);
	flyout.Closed([](const auto&, const auto&) {
		SaveSettings();
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = flyout;

	wchar_t buf[32] = {};
	swprintf(buf, 32, L"%+.1f dB", static_cast<double>(g_volumeBoostDb));
	volumeValue.Text(buf);
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		// If this device is already connected (e.g. the app auto-reconnected
		// on startup and the user then picks it manually), close the stale
		// connection first. Two simultaneous connections to the same device
		// can leave the first one silent.
		auto existing = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (existing != g_audioPlaybackConnections.end())
		{
			existing->second.second.Close();
			// Closing may fire StateChanged(Closed) synchronously, which
			// already erases the entry; re-find before erasing.
			existing = g_audioPlaybackConnections.find(std::wstring(device.Id()));
			if (existing != g_audioPlaybackConnections.end())
				g_audioPlaybackConnections.erase(existing);
		}

		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));

			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					auto it = g_audioPlaybackConnections.find(std::wstring(sender.DeviceId()));
					if (it != g_audioPlaybackConnections.end())
					{
						g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
						g_audioPlaybackConnections.erase(it);
						if (g_audioPlaybackConnections.empty())
							g_volumeBoostMgr.Stop();
					}
					sender.Close();
				}
			});

			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}

	if (success)
	{
		g_lastConnectedDeviceName = std::wstring(device.Name());
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
		if (std::fabs(g_volumeBoostDb) > 0.05f)
			g_volumeBoostMgr.Start(g_volumeBoostDb, device.Name());
	}
	else
	{
		// Closing the connection may fire StateChanged(Closed) synchronously,
		// which already erases the entry, so re-find before erasing.
		if (auto it = g_audioPlaybackConnections.find(std::wstring(device.Id())); it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
			if (g_audioPlaybackConnections.empty())
				g_volumeBoostMgr.Stop();
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	ConnectDevice(picker, device);
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		if (auto it = g_audioPlaybackConnections.find(std::wstring(device.Id())); it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			// Closing may fire StateChanged(Closed) synchronously, which
			// already erases the entry; re-find before erasing.
			it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
			if (it != g_audioPlaybackConnections.end())
				g_audioPlaybackConnections.erase(it);
			if (g_audioPlaybackConnections.empty())
				g_volumeBoostMgr.Stop();
		}
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
		// Dismiss the picker and hide the helper window so the app cannot
		// stay stuck in the foreground after disconnecting.
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
