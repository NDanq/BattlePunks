# NimbleEngine Documentation

The editor's Help > Documentation shows only this folder.

## Guides

- [Demo Lab](../../Games/DemoLab/Docs/Guide.md): the sample game -- running it, what each level and cinematic shows, regenerating its content, its tests and its perf recipe. It is an example archive: the link works once File > New Project > Example: Demo Lab (or `NewProject --example DemoLab`) has unpacked it into `Games/DemoLab`
- Guides built on WorldsLab's worlds (the Shooting Range, GrassLab, the Toon Arena) the same way: unpack it with File > New Project > Example: Worlds Lab (or `NewProject --example WorldsLab`)
- [Game modes: a usage guide](Guides/GameModeGuide.md)
- [The Character Mover](Guides/CharacterMoverGuide.md): the engine's predicted third-person walker, for players and bots
- UI, built on the Shooting Range: [a menu](Guides/UI/MenuUIGuide.md), [a HUD](Guides/UI/HudGuide.md), [settings that control the game's .ini](Guides/UI/SettingsUIGuide.md)
- [Sequencer implementation guide](Guides/SequencerImplementationGuide.md)
- [AICore implementation guide](Guides/AICoreImplementationGuide.md): navigation, crowds, behavior trees, utility, perception
- [Crash dummy animation guide](Guides/CrashDummyAnimationGuide.md)
- [Retargeting](Guides/RetargetGuide.md): a clip pack made for another rig, played on your character, then baked
- [Procedural content: splines and Procedural Spawners](Guides/ProceduralGuide.md), built on Demo Lab's L1 Foothills
- [Importing meshes with meshlets](Guides/MeshletImportGuide.md): building them at import (the wizard or `--import`), turning GPU meshlet culling on, checking it, and when it is worth it; Demo Lab's statue as the example
- [Custom render passes](Guides/CustomRenderPassGuide.md): your own GPU work at hooks in the frame, built on WorldsLab's GrassLab
- [Plugins](Guides/PluginGuide.md): drop-in modules a project enables in one line; a worked example (Spinner), and TimeOfDay and Weather as Demo Lab uses them

## Component reference

One page per component, with its C++ and Lua API: the **?** in a component's Details header opens it, and writes it here first if it is missing.

- [AI Brain](Components/AIBrain.md)
- [AI Stimulus](Components/AIStimulus.md)
- [Anim Socket](Components/AnimSocket.md)
- [Animation](Components/Animation.md)
- [Audio Listener](Components/AudioListener.md)
- [Audio Source](Components/AudioSource.md)
- [AvatarHarness](Components/AvatarHarness.md)
- [Bounds](Components/Bounds.md)
- [Camera](Components/Camera.md)
- [Collision](Components/Collision.md)
- [Directional Light](Components/DirectionalLight.md)
- [Input](Components/Input.md)
- [Nav Agent](Components/NavAgent.md)
- [Nav Mesh Volume](Components/NavMeshVolume.md)
- [Nav Modifier](Components/NavModifier.md)
- [Nav Obstacle](Components/NavObstacle.md)
- [Net](Components/Net.md)
- [Particle System](Components/ParticleSystem.md)
- [Perception](Components/Perception.md)
- [Physics Constraint](Components/PhysicsConstraint.md)
- [PlayerStart](Components/PlayerStart.md)
- [PlayerState](Components/PlayerState.md)
- [Point Light](Components/PointLight.md)
- [Possessor](Components/Possessor.md)
- [Post Process](Components/PostProcess.md)
- [Procedural Spawner](Components/ProceduralSpawner.md)
- [Ragdoll](Components/Ragdoll.md)
- [Reflection Probe](Components/ReflectionProbe.md)
- [Render](Components/Render.md)
- [Rigid Body](Components/RigidBody.md)
- [Scene Capture Probe](Components/SceneCaptureProbe.md)
- [Sequence Player](Components/SequencePlayer.md)
- [Sky Light](Components/SkyLight.md)
- [Spline](Components/Spline.md)
- [Spot Light](Components/SpotLight.md)
- [Terrain](Components/Terrain.md)
- [Transform](Components/Transform.md)
- [Vehicle](Components/Vehicle.md)
