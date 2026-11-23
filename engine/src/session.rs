//! The session (the standard, §3.1): what every text field of a process shares. The settings (§8),
//! the custom layouts (§7.6) and the user dictionary (§11), with what was wrong the last time each
//! was changed.

use std::collections::BTreeMap;
use std::sync::{Arc, RwLock};

use bada_compat::{Change, Harness, Mode, Problem, SessionReply, merge_patch};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::data::{Data, UserDictionary};
use crate::layout::{Layout, stock};

/// How many henkan profiles bada takes.
pub const PROFILES: usize = 64;

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Muhenkan {
    /// A layout that types directly. Unset: every key is passed on.
    pub layout: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct FKeys {
    /// false: every F key goes to the app.
    pub intercept: bool,
    /// F6–F10 → `swallow`, `pass` or `<layout id>.<action id>`.
    pub keys: BTreeMap<String, String>,
}

impl Default for FKeys {
    fn default() -> Self {
        FKeys { intercept: true, keys: BTreeMap::new() }
    }
}

/// What profiles share, and what each may override, field by field (§8.1).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Shared {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub weights: Option<BTreeMap<String, f64>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub script_forms: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub script_forms_layout: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub options: Option<BTreeMap<String, BTreeMap<String, Value>>>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub id: String,
    pub name: String,
    pub layouts: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub weights: Option<BTreeMap<String, f64>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub script_forms: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub script_forms_layout: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub options: Option<BTreeMap<String, BTreeMap<String, Value>>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fkeys: Option<FKeys>,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Henkan {
    pub defaults: Shared,
    pub profiles: Vec<Profile>,
}

/// The `engine` part of the settings file (`bada-settings/2`).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Settings {
    pub muhenkan: Muhenkan,
    pub henkan: Henkan,
}

impl Settings {
    pub fn profile(&self, id: &str) -> Option<&Profile> {
        self.henkan.profiles.iter().find(|p| p.id == id)
    }

    /// A layout's weight in a profile: the profile's, else the defaults', else 1.
    pub fn weight(&self, profile: &Profile, layout: &str) -> f64 {
        let of = |w: &Option<BTreeMap<String, f64>>| w.as_ref().and_then(|w| w.get(layout)).copied();
        of(&profile.weights)
            .or_else(|| of(&self.henkan.defaults.weights))
            .filter(|w| w.is_finite())
            .map_or(1.0, |w| w.clamp(0.0, 10.0))
    }

    /// A layout's option in a profile: the profile's, else the defaults'. None: the layout's own default.
    pub fn option<'a>(&'a self, profile: &'a Profile, layout: &str, name: &str) -> Option<&'a Value> {
        fn of<'a>(
            o: &'a Option<BTreeMap<String, BTreeMap<String, Value>>>,
            layout: &str,
            name: &str,
        ) -> Option<&'a Value> {
            o.as_ref()?.get(layout)?.get(name)
        }
        of(&profile.options, layout, name).or_else(|| of(&self.henkan.defaults.options, layout, name))
    }

    pub fn script_forms(&self, profile: &Profile) -> bool {
        profile.script_forms.or(self.henkan.defaults.script_forms).unwrap_or(false)
    }

    pub fn script_forms_layout<'a>(&'a self, profile: &'a Profile) -> Option<&'a str> {
        profile.script_forms_layout.as_deref().or(self.henkan.defaults.script_forms_layout.as_deref())
    }
}

fn id_is_shaped(id: &str) -> bool {
    (1..=32).contains(&id.len()) && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
}

/// What is wrong with settings, given the layouts there are.
fn check(value: &Value, layouts: &BTreeMap<String, Arc<Layout>>) -> Result<Settings, Vec<Problem>> {
    let settings: Settings = serde_json::from_value(value.clone()).map_err(|e| {
        let message = e.to_string();
        let code = if message.starts_with("unknown field") { "unknown-field" } else { "bad-value" };
        vec![Problem::error("", code, message)]
    })?;
    let mut problems = Vec::new();
    let mut error = |path: String, code: &str, message: String| problems.push(Problem::error(&path, code, message));
    if let Some(id) = &settings.muhenkan.layout {
        match layouts.get(id) {
            None => error("muhenkan.layout".into(), "unknown-layout", format!("no layout named {id}")),
            Some(l) if l.file.kind != Mode::Muhenkan => error(
                "muhenkan.layout".into(),
                "not-muhenkan",
                format!("{id} composes: the muhenkan layout types directly"),
            ),
            Some(_) => {}
        }
    }
    let mut shared = |at: &str,
                      weights: &Option<BTreeMap<String, f64>>,
                      forms: &Option<String>,
                      options: &Option<BTreeMap<String, BTreeMap<String, Value>>>| {
        for (id, w) in weights.iter().flatten() {
            if !layouts.contains_key(id) {
                error(format!("{at}.weights.{id}"), "unknown-layout", format!("no layout named {id}"));
            }
            if !(0.0..=10.0).contains(w) {
                error(format!("{at}.weights.{id}"), "bad-value", "a weight is 0 to 10".into());
            }
        }
        if let Some(id) = forms.as_ref().filter(|id| !layouts.contains_key(*id)) {
            error(format!("{at}.script_forms_layout"), "unknown-layout", format!("no layout named {id}"));
        }
        for (id, set) in options.iter().flatten() {
            let Some(layout) = layouts.get(id) else {
                error(format!("{at}.options.{id}"), "unknown-layout", format!("no layout named {id}"));
                continue;
            };
            for (name, value) in set {
                let path = format!("{at}.options.{id}.{name}");
                match layout.file.options.get(name) {
                    None => error(path, "unknown-option", format!("{id} has no option {name}")),
                    Some(decl) => {
                        let fits = match decl.get("type").and_then(Value::as_str) {
                            Some("boolean") => value.is_boolean(),
                            Some("order") => {
                                let of: Vec<&str> =
                                    decl["of"].as_array().into_iter().flatten().filter_map(Value::as_str).collect();
                                value
                                    .as_array()
                                    .is_some_and(|a| a.iter().all(|v| v.as_str().is_some_and(|s| of.contains(&s))))
                            }
                            _ => true,
                        };
                        if !fits {
                            error(path, "bad-value", format!("not a value {name} can have"));
                        }
                    }
                }
            }
        }
    };
    let d = &settings.henkan.defaults;
    shared("henkan.defaults", &d.weights, &d.script_forms_layout, &d.options);
    let profiles = &settings.henkan.profiles;
    for (i, p) in profiles.iter().enumerate() {
        shared(&format!("henkan.profiles[{i}]"), &p.weights, &p.script_forms_layout, &p.options);
    }
    if profiles.len() > PROFILES {
        error("henkan.profiles".into(), "too-many-profiles", format!("bada takes {PROFILES} profiles"));
    }
    for (i, p) in profiles.iter().enumerate() {
        let at = format!("henkan.profiles[{i}]");
        if !id_is_shaped(&p.id) {
            error(format!("{at}.id"), "bad-value", "an id is 1 to 32 of a-z, 0-9 and -".into());
        }
        if profiles[..i].iter().any(|q| q.id == p.id) {
            error(format!("{at}.id"), "duplicate-profile", format!("two profiles are named {}", p.id));
        }
        if p.name.is_empty() {
            error(format!("{at}.name"), "bad-value", "a profile has a name".into());
        }
        if p.layouts.is_empty() {
            error(format!("{at}.layouts"), "bad-value", "a profile has a layout".into());
        }
        for (j, id) in p.layouts.iter().enumerate() {
            if !layouts.contains_key(id) {
                error(format!("{at}.layouts[{j}]"), "unknown-layout", format!("no layout named {id}"));
            }
            if p.layouts[..j].contains(id) {
                error(format!("{at}.layouts[{j}]"), "bad-value", format!("{id} is listed twice"));
            }
        }
        for (key, bound) in p.fkeys.iter().flat_map(|f| &f.keys) {
            let path = format!("{at}.fkeys.keys.{key}");
            if !["F6", "F7", "F8", "F9", "F10"].contains(&key.as_str()) {
                error(path.clone(), "bad-value", "the F keys are F6 to F10".into());
            }
            if bound != "swallow" && bound != "pass" {
                let known =
                    bound.split_once('.').is_some_and(|(l, a)| layouts.get(l).is_some_and(|l| l.action(a).is_some()));
                if !known {
                    error(path, "unknown-action", format!("no action named {bound}"));
                }
            }
        }
    }
    if problems.is_empty() { Ok(settings) } else { Err(problems) }
}

/// What a text field reads the session through: one consistent view, replaced whole on a change.
#[derive(Debug)]
pub struct Snapshot {
    /// Goes up with every change.
    pub generation: u64,
    pub settings: Settings,
    /// Every layout by id: the stock ones, with the custom ones over them.
    pub layouts: BTreeMap<String, Arc<Layout>>,
    /// The user's own words.
    pub dictionary: Arc<UserDictionary>,
}

impl Snapshot {
    /// A profile's candidates, in order. A layout that is gone is left out.
    pub fn candidates(&self, profile: &str) -> Vec<Arc<Layout>> {
        let Some(p) = self.settings.profile(profile) else { return Vec::new() };
        p.layouts.iter().filter_map(|id| self.layouts.get(id).cloned()).collect()
    }
}

struct State {
    view: Arc<Snapshot>,
    custom: BTreeMap<String, Arc<Layout>>,
    settings_problems: Vec<Problem>,
    layout_problems: Vec<Problem>,
    dictionary_problems: Vec<Problem>,
}

pub struct Session {
    pub harness: Harness,
    /// The data the harness loaded, if any.
    pub data: Arc<Data>,
    state: RwLock<State>,
}

fn all_layouts(custom: &BTreeMap<String, Arc<Layout>>) -> BTreeMap<String, Arc<Layout>> {
    let mut all: BTreeMap<String, Arc<Layout>> = stock().iter().map(|l| (l.id().to_string(), l.clone())).collect();
    for (id, layout) in custom {
        let mut layout = (**layout).clone();
        layout.stock = all.contains_key(id);
        layout.modified = layout.stock;
        all.insert(id.clone(), Arc::new(layout));
    }
    all
}

impl Session {
    pub fn new(harness: &Harness, data: Option<Arc<Data>>) -> Session {
        let view = Arc::new(Snapshot {
            generation: 0,
            settings: Settings::default(),
            layouts: all_layouts(&BTreeMap::new()),
            dictionary: Arc::default(),
        });
        Session {
            harness: harness.clone(),
            data: data.unwrap_or_default(),
            state: RwLock::new(State {
                view,
                custom: BTreeMap::new(),
                settings_problems: Vec::new(),
                layout_problems: Vec::new(),
                dictionary_problems: Vec::new(),
            }),
        }
    }

    pub fn view(&self) -> Arc<Snapshot> {
        self.state.read().unwrap_or_else(|p| p.into_inner()).view.clone()
    }

    /// A `settings` event (§8.2).
    pub fn settings(&self, change: Change, dry: bool) -> SessionReply {
        let mut state = self.state.write().unwrap_or_else(|p| p.into_inner());
        let current = serde_json::to_value(&state.view.settings).unwrap_or_default();
        let wanted = match change {
            Change::Read => {
                return SessionReply {
                    settings: Some(current),
                    problems: state.settings_problems.clone(),
                    ..SessionReply::default()
                };
            }
            Change::Set(value) => value,
            Change::Patch(patch) => {
                let mut value = current.clone();
                merge_patch(&mut value, &patch);
                value
            }
            Change::Remove(_) => Value::Null,
        };
        let (taken, problems) = match check(&wanted, &state.view.layouts) {
            Ok(settings) => (Some(settings), Vec::new()),
            Err(problems) => (None, problems),
        };
        let in_effect = taken.as_ref().map(|s| serde_json::to_value(s).unwrap_or_default()).unwrap_or(current);
        if !dry {
            state.settings_problems = problems.clone();
            if let Some(settings) = taken {
                let old = &state.view;
                state.view = Arc::new(Snapshot {
                    generation: old.generation + 1,
                    settings,
                    layouts: old.layouts.clone(),
                    dictionary: old.dictionary.clone(),
                });
            }
        }
        SessionReply { settings: Some(in_effect), ..SessionReply::default() }.with_problems(problems)
    }

    /// A `layout` event (§7.6).
    pub fn layout(&self, change: Change, dry: bool) -> SessionReply {
        let mut state = self.state.write().unwrap_or_else(|p| p.into_inner());
        // The stock layouts in their own order, as `P_describe` lists them; then the user's, by id.
        let list = |layouts: &BTreeMap<String, Arc<Layout>>| {
            let shipped = stock().iter().filter_map(|s| layouts.get(s.id()));
            let own = layouts.values().filter(|l| !l.stock);
            Some(shipped.chain(own).map(|l| l.info()).collect())
        };
        let mut custom = state.custom.clone();
        let mut problems = Vec::new();
        match change {
            Change::Read => {
                return SessionReply {
                    layouts: list(&state.view.layouts),
                    problems: state.layout_problems.clone(),
                    ..SessionReply::default()
                };
            }
            Change::Set(value) => match Layout::load(&value, false) {
                Ok(layout) => {
                    let id = layout.id().to_string();
                    if stock().iter().any(|l| l.id() == id) {
                        problems.push(Problem::warning(
                            "",
                            "stock-modified",
                            format!("{id} is a stock layout: its key model still expects the stock keys, so beside other layouts the wrong reading may show"),
                        ));
                    } else {
                        problems.push(Problem::warning(
                            "",
                            "no-model",
                            format!(
                                "{id} has no key model: beside other layouts it is ranked by its rank scores alone"
                            ),
                        ));
                    }
                    custom.insert(id, Arc::new(layout));
                }
                Err(found) => problems = found,
            },
            Change::Remove(Some(id)) => {
                if custom.remove(&id).is_none() {
                    problems.push(Problem::error("id", "unknown-layout", format!("no custom layout named {id}")));
                }
            }
            Change::Remove(None) | Change::Patch(_) => {
                problems.push(Problem::error("", "bad-value", "a layout is set or removed"))
            }
        }
        let refused = problems.iter().any(|p| p.level == bada_compat::Level::Error);
        let layouts = if refused { state.view.layouts.clone() } else { all_layouts(&custom) };
        if !dry {
            state.layout_problems = problems.clone();
            if !refused {
                let old = &state.view;
                state.view = Arc::new(Snapshot {
                    generation: old.generation + 1,
                    settings: old.settings.clone(),
                    layouts: layouts.clone(),
                    dictionary: old.dictionary.clone(),
                });
                state.custom = custom;
            }
        }
        SessionReply { layouts: list(&layouts), ..SessionReply::default() }.with_problems(problems)
    }

    /// A `dictionary` event (§11): the user's own words, for every text field of the session.
    pub fn dictionary(&self, change: Change) -> SessionReply {
        let mut state = self.state.write().unwrap_or_else(|p| p.into_inner());
        let summary = |state: &State| Some(state.view.dictionary.summary());
        let (taken, problems) = match change {
            Change::Read => {
                return SessionReply {
                    dictionary: summary(&state),
                    problems: state.dictionary_problems.clone(),
                    ..SessionReply::default()
                };
            }
            Change::Remove(_) => (Some(UserDictionary::default()), Vec::new()),
            Change::Set(value) | Change::Patch(value) => match UserDictionary::parse(&value) {
                Ok(dictionary) => (Some(dictionary), Vec::new()),
                Err(problems) => (None, problems),
            },
        };
        state.dictionary_problems = problems.clone();
        if let Some(dictionary) = taken {
            let old = &state.view;
            state.view = Arc::new(Snapshot {
                generation: old.generation + 1,
                settings: old.settings.clone(),
                layouts: old.layouts.clone(),
                dictionary: Arc::new(dictionary),
            });
        }
        SessionReply { dictionary: summary(&state), ..SessionReply::default() }.with_problems(problems)
    }
}
