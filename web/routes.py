from webserver import WebServer, set_context_processor
import web.views as views
import web.context_processors as context_processors

# Wire the global context processor so every render_template call automatically
# receives theme and other shared context values.
set_context_processor(context_processors.get_context)

# Register views with the web server
web_server = WebServer()

web_server.add_routes(
    {
        "/": views.HomeView,
        "/set_scene": views.SetSceneView,
        "/scenes/panel/status": views.ScenePanelStatusView,
        "/animation": views.AnimationView,
        "/storage": views.StorageView,
        "/setup": views.SetupView,
        "/models": views.ModelsView,
        "/models/summary": views.ModelsSummaryView,
        "/models/set": views.ModelsSetView,
        "/models/wrap": views.ModelsWrapView,
        "/confirm": views.ConfirmView,
        "/status": views.StatusView,
        "/named_range": views.NamedRangeView,
        "/named_range/set": views.NamedRangeSetView,
        "/named_range/remove_subrange": views.NamedRangeRemoveSubrangeView,
        "/named_range/summary": views.NamedRangeSummaryView,
        "/custom_colors": views.CustomColorsView,
        "/custom_colors/summary": views.CustomColorsSummaryView,
        "/scenes": views.ScenesView,
        "/scenes/edit": views.SceneEditView,
        "/scenes/edit/add_trigger_scene": views.SceneEditAddTriggerSceneView,
        "/scenes/edit/remove_trigger_scene": views.SceneEditRemoveTriggerSceneView,
        "/scenes/color_select": views.ColorSelectView,
        "/scenes/summary": views.ScenesSummaryView,
        "/effects": views.EffectsView,
        "/effects/edit": views.EffectEditView,
        "/effects/color_select": views.ColorSelectView,
        "/effects/summary": views.EffectsSummaryView,
        "/filters": views.FiltersView,
        "/filters/edit": views.FilterEditView,
        "/filters/color_select": views.ColorSelectView,
        "/filters/summary": views.FiltersSummaryView,
        "/backup": views.BackupView,
        "/restore": views.RestoreView,
        "/restore/confirm": views.RestoreConfirmView,
        "/theme": views.ThemeView,
        "/theme/delete": views.ThemeDeleteView,
        "/theme/upload": views.ThemeUploadView,
        "/hostname": views.HostnameView,
        "/system_settings": views.SystemSettingsView,
        "/system_settings/summary": views.SystemSettingsSummaryView,
        "/system_settings/ip_announced": views.SystemSettingsIPAnnouncedView,
        "/system_reboot": views.SystemRebootView,
        "/system_reboot/confirm": views.SystemRebootConfirmView,
        "/updates": views.UpdatesView,
        "/updates/summary": views.UpdatesSummaryView,
        "/sounds": views.SoundsView,
        "/sounds/summary": views.SoundsSummaryView,
        "/sounds/edit": views.SoundEditView,
        "/sounds/status": views.SoundsStatusView,
        "/sounds/play": views.PlaySoundView,
        "/sounds/stop": views.StopSoundView,
        "/sounds/stop-all": views.StopAllSoundsView,
        "/soundscapes": views.SoundscapesView,
        "/soundscapes/summary": views.SoundscapesSummaryView,
        "/soundscapes/status": views.SoundscapesStatusView,
        "/soundscapes/edit": views.SoundscapeEditView,
        "/soundscapes/play": views.PlaySoundscapeView,
        "/soundscapes/stop": views.StopSoundscapeView,
        "/audio/volume": views.AudioVolumeView,
    }
)


# Animation runs freely without webserver synchronization
