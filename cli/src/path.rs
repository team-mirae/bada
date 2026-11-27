//! Paths into a JSON file, as the standard writes them: `engine.henkan.profiles[0].layouts`
//! (§10.2). A name is an object's key; `[n]` an array's index.

use serde_json::Value;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Step {
    Key(String),
    Index(usize),
}

/// The steps of a path; `None` when it isn't one. The empty path is the whole file.
pub fn parse(path: &str) -> Option<Vec<Step>> {
    let mut steps = Vec::new();
    if path.is_empty() {
        return Some(steps);
    }
    for part in path.split('.') {
        let (name, mut rest) = match part.find('[') {
            Some(i) => (&part[..i], &part[i..]),
            None => (part, ""),
        };
        if name.is_empty() && steps.is_empty() && rest.is_empty() {
            return None;
        }
        if !name.is_empty() {
            steps.push(Step::Key(name.to_string()));
        } else if rest.is_empty() {
            return None;
        }
        while !rest.is_empty() {
            let end = rest.find(']')?;
            let index = rest.get(1..end)?.parse().ok()?;
            steps.push(Step::Index(index));
            rest = &rest[end + 1..];
            if !rest.is_empty() && !rest.starts_with('[') {
                return None;
            }
        }
    }
    Some(steps)
}

/// The value at `steps`, if there is one.
pub fn get<'a>(value: &'a Value, steps: &[Step]) -> Option<&'a Value> {
    steps.iter().try_fold(value, |v, step| match step {
        Step::Key(k) => v.get(k),
        Step::Index(i) => v.get(*i),
    })
}

/// Sets the value at `steps`, making the objects on the way. An index must name an item there is,
/// or the one after the last (which appends). Returns a message when the path can't be followed.
pub fn set(value: &mut Value, steps: &[Step], new: Value) -> Result<(), String> {
    let Some((last, way)) = steps.split_last() else {
        *value = new;
        return Ok(());
    };
    let mut here = value;
    for (n, step) in way.iter().enumerate() {
        let next_is_index = matches!(steps[n + 1], Step::Index(_));
        here = match step {
            Step::Key(k) => {
                let object = here.as_object_mut().ok_or_else(|| format!("{} isn't an object", shown(&steps[..n])))?;
                object.entry(k.clone()).or_insert_with(|| if next_is_index { Value::Array(vec![]) } else { Value::Object(Default::default()) })
            }
            Step::Index(i) => {
                let array = here.as_array_mut().ok_or_else(|| format!("{} isn't a list", shown(&steps[..n])))?;
                array.get_mut(*i).ok_or_else(|| format!("{} has no item {i}", shown(&steps[..n])))?
            }
        };
    }
    match last {
        Step::Key(k) => {
            let object = here.as_object_mut().ok_or_else(|| format!("{} isn't an object", shown(way)))?;
            object.insert(k.clone(), new);
        }
        Step::Index(i) => {
            let array = here.as_array_mut().ok_or_else(|| format!("{} isn't a list", shown(way)))?;
            if *i < array.len() {
                array[*i] = new;
            } else if *i == array.len() {
                array.push(new);
            } else {
                return Err(format!("{} has {} items: no item {i}", shown(way), array.len()));
            }
        }
    }
    Ok(())
}

/// Removes the value at `steps`. Returns whether there was one.
pub fn remove(value: &mut Value, steps: &[Step]) -> bool {
    let Some((last, way)) = steps.split_last() else { return false };
    let mut here = value;
    for step in way {
        here = match step {
            Step::Key(k) => match here.get_mut(k) {
                Some(v) => v,
                None => return false,
            },
            Step::Index(i) => match here.get_mut(*i) {
                Some(v) => v,
                None => return false,
            },
        };
    }
    match last {
        Step::Key(k) => here.as_object_mut().is_some_and(|o| o.remove(k).is_some()),
        Step::Index(i) => match here.as_array_mut() {
            Some(a) if *i < a.len() => {
                a.remove(*i);
                true
            }
            _ => false,
        },
    }
}

/// A path as written.
pub fn shown(steps: &[Step]) -> String {
    let mut out = String::new();
    for step in steps {
        match step {
            Step::Key(k) => {
                if !out.is_empty() {
                    out.push('.');
                }
                out.push_str(k);
            }
            Step::Index(i) => out.push_str(&format!("[{i}]")),
        }
    }
    if out.is_empty() { "the file".into() } else { out }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn paths_read_as_the_standard_writes_them() {
        assert_eq!(
            parse("engine.henkan.profiles[0].layouts").unwrap(),
            vec![
                Step::Key("engine".into()),
                Step::Key("henkan".into()),
                Step::Key("profiles".into()),
                Step::Index(0),
                Step::Key("layouts".into())
            ]
        );
        assert_eq!(parse("a[1][2]").unwrap(), vec![Step::Key("a".into()), Step::Index(1), Step::Index(2)]);
        assert_eq!(parse("").unwrap(), vec![]);
        for bad in ["a..b", "a[x]", "a[1", "a[1]b", "."] {
            assert!(parse(bad).is_none(), "{bad}");
        }
    }

    #[test]
    fn set_makes_the_way_and_appends() {
        let mut v = json!({});
        set(&mut v, &parse("keys.cycle").unwrap(), json!(["ko"])).unwrap();
        set(&mut v, &parse("keys.cycle[1]").unwrap(), json!("ja")).unwrap();
        assert_eq!(v, json!({"keys": {"cycle": ["ko", "ja"]}}));
        assert!(set(&mut v, &parse("keys.cycle[5]").unwrap(), json!("x")).is_err());
        assert_eq!(get(&v, &parse("keys.cycle[1]").unwrap()), Some(&json!("ja")));
        assert!(remove(&mut v, &parse("keys.cycle[0]").unwrap()));
        assert_eq!(v, json!({"keys": {"cycle": ["ja"]}}));
    }
}
