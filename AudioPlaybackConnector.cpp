#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
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
	case WM_DESTROY:
		VolumeBoost::Reset();
		for (const auto& connection : g_audioPlaybackConnections)
		{
			connection.second.second.Close();
			g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}
		if (g_reconnect)
		{
			SaveSettings();
			g_audioPlaybackConnections.clear();
		}
		else
		{
			g_audioPlaybackConnections.clear();
			SaveSettings();
		}
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
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_CONTEXTMENU:
		{
			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

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

	g_xamlFlyout = flyout;
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

// A plain clickable menu row (icon + label) with hover feedback.
static Button MakeMenuRow(const wchar_t* text, const wchar_t* glyph, SolidColorBrush textBrush, SolidColorBrush hoverBrush, winrt::Windows::UI::Xaml::RoutedEventHandler handler)
{
	FontIcon icon;
	icon.Glyph(glyph);
	icon.FontSize(14);
	icon.VerticalAlignment(VerticalAlignment::Center);

	TextBlock label;
	label.Text(_(text));
	label.Foreground(textBrush);
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
	button.Padding({ 8, 6, 8, 6 });
	button.Click(handler);
	button.PointerEntered([hoverBrush](winrt::Windows::Foundation::IInspectable const& sender, winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs const&) {
		sender.as<Button>().Background(hoverBrush);
	});
	button.PointerExited([](winrt::Windows::Foundation::IInspectable const& sender, winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs const&) {
		sender.as<Button>().Background(nullptr);
	});
	return button;
}

void SetupMenu()
{
	const bool light = IsLightTheme();

	SolidColorBrush bgBrush;
	bgBrush.Color(Argb(255, light ? 243 : 43, light ? 243 : 43, light ? 243 : 43));
	SolidColorBrush borderBrush;
	borderBrush.Color(Argb(255, light ? 224 : 60, light ? 224 : 60, light ? 224 : 60));
	SolidColorBrush hoverBrush;
	hoverBrush.Color(Argb(255, light ? 229 : 52, light ? 229 : 52, light ? 229 : 52));
	SolidColorBrush textBrush;
	textBrush.Color(Argb(255, light ? 27 : 243, light ? 27 : 243, light ? 27 : 243));
	SolidColorBrush subtextBrush;
	subtextBrush.Color(Argb(255, light ? 96 : 170, light ? 96 : 170, light ? 96 : 170));

	auto settingsRow = MakeMenuRow(_(L"Bluetooth Settings"), L"\xE713", textBrush, hoverBrush, [](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	// Volume section: caption with a live level readout on the right, and a
	// small linear 0..+30 dB slider below.
	Grid volumeHeader;
	volumeHeader.Margin({ 8, 6, 8, 0 });
	ColumnDefinition colCaption;
	colCaption.Width(GridLength{ 1, GridUnitType::Star });
	ColumnDefinition colValue;
	colValue.Width(GridLength{ 1, GridUnitType::Auto });
	volumeHeader.ColumnDefinitions().Append(colCaption);
	volumeHeader.ColumnDefinitions().Append(colValue);

	TextBlock volumeCaption;
	volumeCaption.Text(_(L"Volume gain"));
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
	slider.Maximum(30);
	slider.Value(g_volumeLevelDb);
	slider.StepFrequency(0.5);
	slider.Margin({ 8, 2, 8, 2 });
	slider.ValueChanged([volumeValue](const auto&, const auto& args) {
		g_volumeLevelDb = static_cast<float>(args.NewValue());
		VolumeBoost::SetLevel(g_volumeLevelDb);
		wchar_t buf[32] = {};
		swprintf(buf, 32, L"%+.1f dB", static_cast<double>(g_volumeLevelDb));
		volumeValue.Text(buf);
	});

	auto exitRow = MakeMenuRow(_(L"Exit"), L"\xE8BB", textBrush, hoverBrush, [](const auto&, const auto&) {
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
		separator.Margin({ 8, 4, 8, 4 });
		return separator;
	};

	StackPanel panel;
	panel.Children().Append(settingsRow);
	panel.Children().Append(makeSeparator());
	panel.Children().Append(volumeHeader);
	panel.Children().Append(slider);
	panel.Children().Append(makeSeparator());
	panel.Children().Append(exitRow);

	Border border;
	border.Background(bgBrush);
	border.BorderBrush(borderBrush);
	border.BorderThickness({ 1, 1, 1, 1 });
	border.Padding({ 6, 4, 6, 4 });
	border.Width(230);
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
	swprintf(buf, 32, L"%+.1f dB", static_cast<double>(g_volumeLevelDb));
	volumeValue.Text(buf);
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
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
							VolumeBoost::Reset();
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
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
		VolumeBoost::SetLevel(g_volumeLevelDb);
	}
	else
	{
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
			if (g_audioPlaybackConnections.empty())
				VolumeBoost::Reset();
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
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
			if (g_audioPlaybackConnections.empty())
				VolumeBoost::Reset();
		}
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
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
