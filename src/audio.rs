use std::collections::{HashMap, HashSet};
use std::io;
use std::process::Command;

use serde_json::Value;

#[derive(Debug, Default, Eq, PartialEq)]
pub struct AudioRoute {
    pub playback: Option<String>,
    pub capture: Option<String>,
    pub laptop_verified: bool,
}

pub fn observe_pipewire() -> io::Result<AudioRoute> {
    let output = Command::new("pw-dump").output()?;
    if !output.status.success() {
        return Err(io::Error::other("PipeWire snapshot unavailable"));
    }
    let json = String::from_utf8(output.stdout).map_err(|_| io::Error::other("PipeWire returned invalid text"))?;
    parse_pipewire_snapshot(&json).map_err(|_| io::Error::other("PipeWire returned invalid graph data"))
}

pub fn parse_pipewire_snapshot(json: &str) -> Result<AudioRoute, serde_json::Error> {
    let objects: Vec<Value> = serde_json::from_str(json)?;
    struct Node<'a> {
        class: &'a str,
        label: Option<&'a str>,
        link_group: Option<&'a str>,
        virtual_node: bool,
    }
    let mut nodes = HashMap::new();
    let mut hfp_playback = Vec::new();
    let mut hfp_capture = Vec::new();
    let mut hfp_device = HashMap::new();
    for object in &objects {
        if object["type"] != "PipeWire:Interface:Node" {
            continue;
        }
        let Some(id) = object["id"].as_u64() else { continue };
        let props = &object["info"]["props"];
        let class = props["media.class"].as_str().unwrap_or("");
        if props["api.bluez5.profile"] == "headset-audio-gateway" {
            match class {
                "Stream/Output/Audio" => hfp_playback.push(id),
                "Stream/Input/Audio" => hfp_capture.push(id),
                _ => {}
            }
            hfp_device.insert(id, props["device.id"].as_u64());
        }
        nodes.insert(id, Node {
            class,
            label: props["node.description"].as_str().or_else(|| props["node.name"].as_str()),
            link_group: props["node.link-group"].as_str(),
            virtual_node: props["node.virtual"] == true || props["node.virtual"] == "true",
        });
    }
    let mut route = AudioRoute::default();
    // A second HFP stream makes the owner ambiguous, so leave the route unverified.
    if hfp_playback.len() != 1 || hfp_capture.len() != 1 {
        return Ok(route);
    }
    let playback_device = hfp_device.get(&hfp_playback[0]).copied().flatten();
    let capture_device = hfp_device.get(&hfp_capture[0]).copied().flatten();
    if playback_device.is_none() || playback_device != capture_device {
        return Ok(route);
    }
    let active_links: Vec<_> = objects.iter().filter_map(|object| {
        if object["type"] != "PipeWire:Interface:Link" || object["info"]["state"] != "active" {
            return None;
        }
        Some((object["info"]["output-node-id"].as_u64()?, object["info"]["input-node-id"].as_u64()?))
    }).collect();
    for &(output, input) in &active_links {
        if output == hfp_playback[0] {
            if let Some(Node { class: "Audio/Sink", label: Some(label), .. }) = nodes.get(&input) {
                route.playback = Some((*label).to_owned());
            }
        }
        if input == hfp_capture[0] {
            if let Some(Node { class: "Audio/Source", label: Some(label), link_group, virtual_node }) = nodes.get(&output) {
                if !virtual_node {
                    route.capture = Some((*label).to_owned());
                } else if let Some(group) = link_group.filter(|group| !group.is_empty()) {
                    let filter_inputs: HashSet<_> = nodes.iter().filter_map(|(&id, node)|
                        (node.class == "Stream/Input/Audio" && node.link_group == Some(group)).then_some(id)
                    ).collect();
                    let physical_sources: HashSet<_> = active_links.iter().filter_map(|&(source, target)| {
                        (filter_inputs.contains(&target) && nodes.get(&source).is_some_and(|node|
                            node.class == "Audio/Source" && !node.virtual_node)).then_some(source)
                    }).collect();
                    if physical_sources.len() == 1 {
                        route.capture = physical_sources.iter().next().and_then(|id| nodes.get(id))
                            .and_then(|node| node.label).map(str::to_owned);
                    }
                }
            }
        }
    }
    route.laptop_verified = route.playback.is_some() && route.capture.is_some();
    Ok(route)
}

#[cfg(test)]
mod tests {
    use super::*;

    const ROUTED: &str = r#"[
      {"id":1,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Output/Audio","api.bluez5.profile":"headset-audio-gateway","device.id":99}}},
      {"id":2,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Input/Audio","api.bluez5.profile":"headset-audio-gateway","device.id":99}}},
      {"id":3,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Sink","node.description":"Wired output"}}},
      {"id":4,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Source","node.description":"Wired input"}}},
      {"id":5,"type":"PipeWire:Interface:Link","info":{"output-node-id":1,"input-node-id":3,"state":"active"}},
      {"id":6,"type":"PipeWire:Interface:Link","info":{"output-node-id":4,"input-node-id":2,"state":"active"}}
    ]"#;

    #[test]
    fn verifies_both_audio_directions() {
        let route = parse_pipewire_snapshot(ROUTED).unwrap();
        assert_eq!(route.playback.as_deref(), Some("Wired output"));
        assert_eq!(route.capture.as_deref(), Some("Wired input"));
        assert!(route.laptop_verified);
    }

    #[test]
    fn one_direction_never_claims_working_audio() {
        let without_mic = ROUTED.replace("\"output-node-id\":4,\"input-node-id\":2", "\"output-node-id\":4,\"input-node-id\":3");
        let route = parse_pipewire_snapshot(&without_mic).unwrap();
        assert_eq!(route.playback.as_deref(), Some("Wired output"));
        assert_eq!(route.capture, None);
        assert!(!route.laptop_verified);
    }

    #[test]
    fn no_hfp_streams_means_unverified_route() {
        assert_eq!(parse_pipewire_snapshot("[]").unwrap(), AudioRoute::default());
    }

    #[test]
    fn hfp_directions_from_different_phones_are_unverified() {
        let mixed = ROUTED.replacen("\"device.id\":99", "\"device.id\":100", 1);
        assert!(!parse_pipewire_snapshot(&mixed).unwrap().laptop_verified);
    }

    #[test]
    fn virtual_mic_needs_a_live_physical_source() {
        let filtered = r#"[
          {"id":1,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Output/Audio","api.bluez5.profile":"headset-audio-gateway","device.id":99}}},
          {"id":2,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Input/Audio","api.bluez5.profile":"headset-audio-gateway","device.id":99}}},
          {"id":3,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Sink","node.description":"Wired output"}}},
          {"id":4,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Source","node.description":"Wired input"}}},
          {"id":7,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Source","node.description":"Filtered input","node.virtual":true,"node.link-group":"filter-1"}}},
          {"id":8,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Input/Audio","node.virtual":true,"node.link-group":"filter-1"}}},
          {"id":5,"type":"PipeWire:Interface:Link","info":{"output-node-id":1,"input-node-id":3,"state":"active"}},
          {"id":6,"type":"PipeWire:Interface:Link","info":{"output-node-id":7,"input-node-id":2,"state":"active"}},
          {"id":9,"type":"PipeWire:Interface:Link","info":{"output-node-id":4,"input-node-id":8,"state":"active"}}
        ]"#;
        let route = parse_pipewire_snapshot(filtered).unwrap();
        assert_eq!(route.capture.as_deref(), Some("Wired input"));
        assert!(route.laptop_verified);

        let disconnected = filtered.replace("\"output-node-id\":4,\"input-node-id\":8", "\"output-node-id\":4,\"input-node-id\":3");
        assert!(!parse_pipewire_snapshot(&disconnected).unwrap().laptop_verified);
    }

    #[test]
    #[ignore = "requires a user-routed active phone call"]
    fn live_route_has_playback_and_capture() {
        let route = observe_pipewire().unwrap();
        assert!(route.laptop_verified, "both laptop audio directions must be linked");
    }
}
