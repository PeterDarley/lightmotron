"""Aggregator that exposes every web view class under a single module.

The view implementations live in per-feature modules (``views_home``,
``views_scenes``, ``views_effects``, ...). ``web/routes.py`` resolves all
views through this module as ``views.ClassName``, so each feature module's
public view classes are re-exported here. Shared helpers and the ``lights``
singleton live in ``views_common``.
"""

from web import views_named_ranges
from web.views_common import lights, summarize_led_list, _rename_named_range_refs

# Named range views are grouped into a dedicated module by feature area.
views_named_ranges.initialize_named_range_views(
    lights_instance=lights,
    rename_named_range_refs_func=_rename_named_range_refs,
    summarize_led_list_func=summarize_led_list,
)
NamedRangeSummaryView = views_named_ranges.NamedRangeSummaryView
NamedRangeView = views_named_ranges.NamedRangeView
NamedRangeSetView = views_named_ranges.NamedRangeSetView
NamedRangeRemoveSubrangeView = views_named_ranges.NamedRangeRemoveSubrangeView

from web.views_home import (
    HomeView,
    SetSceneView,
    ScenePanelStatusView,
    AnimationView,
)
from web.views_models import (
    ModelsSummaryView,
    ModelsView,
    ModelsSetView,
    ModelsWrapView,
)
from web.views_soundscapes import (
    SoundscapesSummaryView,
    SoundscapesView,
    PlaySoundscapeView,
    StopSoundscapeView,
    SoundscapesStatusView,
    SoundscapeEditView,
)
from web.views_storage import (
    StorageView,
    BackupView,
    RestoreView,
    RestoreConfirmView,
)
from web.views_colors import (
    CustomColorsSummaryView,
    CustomColorsView,
    ColorSelectView,
)
from web.views_system import (
    SetupView,
    StatusView,
    ConfirmView,
    ThemeView,
    ThemeDeleteView,
    ThemeUploadView,
    HostnameView,
    UpdatesSummaryView,
    UpdatesView,
    SystemSettingsSummaryView,
    SystemSettingsView,
    SystemSettingsIPAnnouncedView,
    SystemRebootView,
    SystemRebootConfirmView,
)
from web.views_scenes import (
    ScenesSummaryView,
    ScenesView,
    SceneEditView,
    SceneEditAddTriggerSceneView,
    SceneEditRemoveTriggerSceneView,
)
from web.views_effects import (
    EffectsSummaryView,
    EffectsView,
    EffectEditView,
)
from web.views_filters import (
    FiltersSummaryView,
    FiltersView,
    FilterEditView,
)
from web.views_sounds import (
    AudioVolumeView,
    SoundsSummaryView,
    SoundsView,
    SoundEditView,
    PlaySoundView,
    StopSoundView,
    StopAllSoundsView,
    SoundsStatusView,
)
