/*
 * UITests -- the UI module headless: the widget registry, layout at several
 * viewports, text, styles, XML and cooked files, actions, bindings and input
 * routing, and the per-frame cost of an unchanged document. No GPU and no
 * window: draw lists are inspected, not drawn (RenderCore's smoke test draws
 * one on a GPU).
 *
 * Plain Expect() checks, like the engine's other tests; non-zero on failure.
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderView.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UIFile.h"
#include "Public/UI/UIFont.h"
#include "Public/UI/UIProperty.h"
#include "Public/UI/UISystem.h"
#include "Public/UI/UIWidgets.h"

using namespace UI;

// === Allocation counting ======================================================
// Every operator new in the process: what "an unchanged frame allocates
// nothing" is measured against.

static std::atomic<uint64_t> GAllocations{ 0 };

void* operator new(const size_t InBytes)
{
    GAllocations.fetch_add(1, std::memory_order_relaxed);
    if(void* Block = std::malloc(InBytes ? InBytes : 1)) return Block;
    throw std::bad_alloc();
}
void* operator new[](const size_t InBytes) { return operator new(InBytes); }
void  operator delete(void* InBlock) noexcept { std::free(InBlock); }
void  operator delete[](void* InBlock) noexcept { std::free(InBlock); }
void  operator delete(void* InBlock, size_t) noexcept { std::free(InBlock); }
void  operator delete[](void* InBlock, size_t) noexcept { std::free(InBlock); }

// === Checks ===================================================================

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* InWhat)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL: %s\n", InWhat);
    }
}

static bool Near(const float InA, const float InB, const float InTolerance = 0.01f)
{
    return std::fabs(InA - InB) <= InTolerance;
}

static int GWarnings = 0;
static void TestLogHandler(const EngineCore::LogVerbosity InVerbosity, const char* InMessage)
{
    if(InVerbosity != EngineCore::LogVerbosity::Log) { ++GWarnings; }
    if(std::getenv("UI_TESTS_VERBOSE")) { std::printf("    log: %s\n", InMessage); }
}

/* InXml parsed into an asset, or an empty one (and a failed check). */
static std::shared_ptr<UIDocumentAsset> Parse(const std::string& InXml, const char* InWhat)
{
    auto Asset = std::make_shared<UIDocumentAsset>();
    std::string Error;
    const bool bParsed = ParseUIXml(InXml, *Asset, &Error);
    if(!bParsed) { std::printf("  parse error (%s): %s\n", InWhat, Error.c_str()); }
    Expect(bParsed, InWhat);
    return Asset;
}

// === Registry =================================================================

static void TestRegistry()
{
    std::printf("[registry]\n");

    const UIWidgetType* Button = FindWidgetType("Button");
    const UIWidgetType* Widget = FindWidgetType("Widget");
    Expect(Button && Widget && Button->IsA(*Widget) && !Widget->IsA(*Button), "Button derives from Widget");
    Expect(Button && !Button->IsAbstract() && Widget && Widget->IsAbstract(), "Widget is abstract, Button is not");
    Expect(FindWidgetType("BoxWidget") && FindWidgetType("BoxWidget")->IsAbstract(), "the boxes' shared base cannot be made");
    Expect(Button && Button->FindProperty("Opacity") >= 0 && Button->FindProperty("Padding") >= 0
           && Button->FindProperty("Normal.Tint") >= 0 && Button->FindProperty("OnClick") >= 0,
           "a Button has Widget's, ContentWidget's and its own properties, brushes flattened");
    Expect(Button && Button->FindProperty("Opacity") < Button->FindProperty("Padding")
           && Button->FindProperty("Padding") < Button->FindProperty("Normal.Tint"),
           "a type's properties come after its parent's");
    Expect(FindWidgetTypeByClass("UI::UIButton") == Button, "a class is found by its bare name");
    Expect(Button && Button->MaxChildren == 1 && FindWidgetType("Canvas")->MaxChildren == -1 && FindWidgetType("Image")->MaxChildren == 0,
           "child limits: a Button takes one, a Canvas any, an Image none");

    int Palette = 0;
    for(const UIWidgetType* Each : GetWidgetTypes()) { if(!Each->Palette.empty()) ++Palette; }
    Expect(Palette == 14, "the fourteen widgets are offered in the palette");

    const UIWidgetType* Text = FindWidgetType("Text");
    const int32_t TextProperty = Text ? Text->FindProperty("Text") : -1;
    Expect(TextProperty >= 0 && (Text->Properties[size_t(TextProperty)].Flags & UPF_Bindable), "Text.Text is bindable");
}

// === Values and actions =======================================================

static void TestValuesAndActions()
{
    std::printf("[values and actions]\n");

    UIColor Color;
    Expect(ParseColor("#FF800040", Color) && Near(Color.R, 1.0f) && Near(Color.G, 128.0f / 255.0f) && Near(Color.A, 64.0f / 255.0f),
           "#RRGGBBAA parses");
    Expect(ParseColor("#0F0", Color) && Near(Color.G, 1.0f) && Near(Color.A, 1.0f), "#RGB parses, alpha 1");
    Expect(!ParseColor("FF0000", Color) && !ParseColor("#12345", Color), "bad colours are refused");
    Expect(FormatColor(UIColor{ 1, 0, 0, 1 }) == "#FF0000FF", "colours write as #RRGGBBAA");

    UIMargin Margin;
    Expect(ParseMargin("4", Margin) && Margin.Left == 4 && Margin.Bottom == 4, "one value is every side");
    Expect(ParseMargin("4,8", Margin) && Margin.Left == 4 && Margin.Top == 8 && Margin.Right == 4 && Margin.Bottom == 8, "two are horizontal, vertical");
    Expect(ParseMargin("1, 2, 3, 4", Margin) && Margin.Right == 3, "four are left, top, right, bottom, spaces allowed");
    Expect(!ParseMargin("1,2,3", Margin), "three values are refused");
    Expect(FormatMargin(UIMargin{ 2, 2, 2, 2 }) == "2" && FormatMargin(UIMargin{ 1, 2, 1, 2 }) == "1,2", "margins write short when they can");
    Expect(FormatFloat(0.1f) == "0.1" && FormatFloat(3.0f) == "3", "floats write their shortest exact form");

    UIActionList List;
    std::string Error;
    Expect(List.Parse("Native:Game.Quit", &Error) && List.Actions.size() == 1 && List.Actions[0].Kind == EUIActionKind::Native
           && List.Actions[0].Target == "Game.Quit" && List.Actions[0].Args.Count == 0, "a native action parses");
    Expect(List.Parse("Lua:Menu.Select('Arena', 2, true); Event:Menu.Changed($self, $value)", &Error) && List.Actions.size() == 2,
           "two actions with arguments parse");
    if(List.Actions.size() == 2)
    {
        const UIAction& Lua = List.Actions[0];
        Expect(Lua.Kind == EUIActionKind::Lua && Lua.Args.Count == 3
               && std::string(Lua.Args.Args[0].String) == "Arena" && Lua.Args.Args[1].Number == 2.0 && Lua.Args.Args[2].Bool,
               "a string, a number and a bool");
        Expect(List.Actions[1].Tokens[0] == UIAction::AT_Self && List.Actions[1].Tokens[1] == UIAction::AT_Value, "$self and $value are tokens");
    }
    Expect(List.Parse("", &Error) && List.IsEmpty(), "empty text is no actions");
    Expect(!List.Parse("Native Game.Quit", &Error) && List.IsEmpty() && List.Source == "Native Game.Quit", "a missing ':' fails, the text kept");
    Expect(!List.Parse("Python:Run", &Error), "an unknown kind fails");
    Expect(!List.Parse("Lua:F(1,2,3,4,5)", &Error), "five arguments fail");
    Expect(!List.Parse("Lua:F(Arena)", &Error), "an unquoted word fails");
    Expect(!List.Parse("Lua:F('open", &Error), "an unclosed string fails");
    Expect(!List.Parse("Lua:A Lua:B", &Error), "actions need a ';' between");
}

// === Layout ===================================================================

static const char* const LAYOUT_XML = R"XML(<UI Format="1" ReferenceSize="1920,1080" ScaleMode="FitHeight" InputMode="GameAndUI">
  <Canvas Name="Root">
    <Border Name="TopLeft" Anchors="0,0,0,0" Position="32,32" Size="200,100"/>
    <Border Name="Centred" Anchors="0.5,0.5,0.5,0.5" Pivot="0.5,0.5" Size="400,200"/>
    <Border Name="Stretched" Anchors="0,1,1,1" Pivot="0,1" Size="0,50" Margin="10,0,10,0"/>
    <Border Name="BottomRight" Anchors="1,1,1,1" Pivot="1,1" Position="-16,-16" Size="64,64"/>
    <HorizontalBox Name="Row" Anchors="0,0,0,0" Position="0,500" Size="1000,40" Spacing="10">
      <Spacer Name="Fixed" Size="100,20"/>
      <Spacer Name="One" Fill="1"/>
      <Spacer Name="Two" Fill="2" VAlign="Center" Size="0,20"/>
      <Spacer Name="Gone" Visibility="Collapsed" Size="500,500"/>
    </HorizontalBox>
    <Overlay Name="Stack" Anchors="0,0,0,0" Position="0,600" Size="300,300">
      <Spacer Name="Corner" HAlign="Right" VAlign="Bottom" Size="50,40" Margin="5"/>
      <Spacer Name="Hidden" Visibility="Hidden" HAlign="Left" VAlign="Top" Size="10,10"/>
    </Overlay>
    <Border Name="Auto" Anchors="0,0,0,0" Position="1500,0" AutoSize="true" Padding="10">
      <Spacer Size="30,20"/>
    </Border>
  </Canvas>
</UI>)XML";

static UIRect RectOf(const UIDocumentInstance& InDoc, const char* InName)
{
    const UIWidget* W = InDoc.FindWidget(InName);
    return W ? InDoc.GetRect(W->GetIndex()) : UIRect{ -1, -1, -1, -1 };
}

static void TestLayout()
{
    std::printf("[layout]\n");
    auto Asset = Parse(LAYOUT_XML, "the layout document parses");
    UIDocumentInstance Doc(Asset);
    Expect(Doc.IsValid() && Doc.GetWidgetCount() == 15, "the layout document instances, every widget");

    Doc.Layout(UIVec2{ 1920, 1080 });
    Expect(Near(Doc.GetScale(), 1.0f) && Near(Doc.GetLayoutSize().X, 1920), "1080p: scale 1");
    UIRect R = RectOf(Doc, "TopLeft");
    Expect(Near(R.X, 32) && Near(R.Y, 32) && Near(R.W, 200) && Near(R.H, 100), "a point-anchored child sits at its Position, its Size");
    R = RectOf(Doc, "Centred");
    Expect(Near(R.X, 760) && Near(R.Y, 440) && Near(R.W, 400), "a centre-anchored child with pivot 0.5 is centred");
    R = RectOf(Doc, "Stretched");
    Expect(Near(R.X, 10) && Near(R.W, 1900) && Near(R.Y, 1030) && Near(R.H, 50), "a stretched axis spans its anchors less Margin");
    R = RectOf(Doc, "BottomRight");
    Expect(Near(R.X, 1920 - 16 - 64) && Near(R.Y, 1080 - 16 - 64), "pivot 1,1 at the corner, Position inward");

    /* The row: 1000 wide, 100 fixed + 2 gaps of 10 (the collapsed one takes none); 880 shared 1:2. */
    R = RectOf(Doc, "Fixed");
    Expect(Near(R.X, 0) && Near(R.W, 100) && Near(R.H, 40), "a box's fixed child is its desired width, full height");
    R = RectOf(Doc, "One");
    Expect(Near(R.X, 110) && Near(R.W, 880.0f / 3.0f, 0.1f), "Fill 1 takes a third of what is left");
    R = RectOf(Doc, "Two");
    Expect(Near(R.W, 880.0f * 2.0f / 3.0f, 0.1f) && Near(R.H, 20) && Near(R.Y, 510), "Fill 2 takes two thirds; VAlign Center centres it");
    R = RectOf(Doc, "Gone");
    Expect(R.W == 0 && R.H == 0, "a collapsed child takes no space");

    R = RectOf(Doc, "Corner");
    Expect(Near(R.X, 300 - 5 - 50) && Near(R.Y, 600 + 300 - 5 - 40) && Near(R.W, 50), "an Overlay aligns its child by HAlign, VAlign, Margin");
    R = RectOf(Doc, "Hidden");
    Expect(Near(R.W, 10) && Doc.IsShown(Doc.FindWidget("Corner")->GetIndex()) && !Doc.IsShown(Doc.FindWidget("Hidden")->GetIndex()),
           "a hidden child keeps its place but is not shown");
    R = RectOf(Doc, "Auto");
    Expect(Near(R.W, 50) && Near(R.H, 40), "AutoSize: its desired size, padding included");

    /* 1440p: the same layout at 4/3 scale. */
    Expect(Doc.Layout(UIVec2{ 2560, 1440 }), "a new viewport lays out again");
    Expect(Near(Doc.GetScale(), 1440.0f / 1080.0f) && Near(Doc.GetLayoutSize().X, 1920), "1440p: scale 4/3, same layout size");
    Expect(!Doc.Layout(UIVec2{ 2560, 1440 }), "the same viewport, nothing changed: no layout");

    /* Ultrawide, FitHeight: wider layout space; the stretched bar follows. */
    Doc.Layout(UIVec2{ 3440, 1440 });
    Expect(Near(Doc.GetLayoutSize().X, 3440.0f * 1080.0f / 1440.0f, 0.1f), "ultrawide: the width follows the aspect");
    R = RectOf(Doc, "Stretched");
    Expect(Near(R.W, Doc.GetLayoutSize().X - 20, 0.1f), "a stretched child spans the wider space");

    /* 720p under each scale mode. */
    struct Mode { const char* Name; float Width, Height, Scale; };
    const Mode Modes[] = {
        { "FitHeight", 1280, 720, 720.0f / 1080.0f }, { "FitWidth", 1280, 1024, 1280.0f / 1920.0f },
        { "Fit", 1280, 1024, 1280.0f / 1920.0f },     { "Fill", 1280, 1024, 1024.0f / 1080.0f }, { "None", 1280, 720, 1.0f },
    };
    for(const Mode& Each : Modes)
    {
        auto Scaled = std::make_shared<UIDocumentAsset>(*Asset);
        Scaled->Settings.SetFromText("ScaleMode", Each.Name);
        UIDocumentInstance ScaledDoc(Scaled);
        ScaledDoc.Layout(UIVec2{ Each.Width, Each.Height });
        const std::string What = std::string("ScaleMode ") + Each.Name;
        Expect(Near(ScaledDoc.GetScale(), Each.Scale, 0.001f), What.c_str());
    }

    /* A layout property relays out; a paint property only repaints. */
    Doc.Layout(UIVec2{ 1920, 1080 });
    Doc.Paint();
    UIWidget* TopLeft = Doc.FindWidget("TopLeft");
    Expect(Doc.SetPropertyText(*TopLeft, "Opacity", "0.5") && Doc.IsPaintDirty() && !Doc.IsLayoutDirty(), "Opacity repaints only");
    Expect(Doc.SetPropertyText(*TopLeft, "Position", "40,40") && Doc.IsLayoutDirty(), "Position relays out");
    Doc.Layout(UIVec2{ 1920, 1080 });
    Expect(Near(RectOf(Doc, "TopLeft").X, 40), "the new Position is laid out");
    Expect(!Doc.SetPropertyText(*TopLeft, "NoSuchProperty", "1") && !Doc.SetPropertyText(*TopLeft, "Opacity", "loud"),
           "an unknown property or a bad value is refused");
}

// === Paint ====================================================================

static void TestPaint()
{
    std::printf("[paint]\n");
    auto Asset = Parse(R"XML(<UI ReferenceSize="100,100" ScaleMode="None">
  <Canvas>
    <Border Name="A" Size="10,10" Background.Tint="#FF0000FF"/>
    <Border Name="B" Position="20,0" Size="10,10" Background.Tint="#00FF00FF" ZOrder="-1"/>
    <Border Name="Clip" Position="0,50" Size="20,20" ClipChildren="true" Background.DrawAs="None">
      <Border Name="Inner" Size="100,100" Background.Tint="#0000FFFF"/>
    </Border>
    <Border Name="Faded" Position="50,50" Size="10,10" Opacity="0.5" Background.Tint="#FFFFFFFF"/>
    <Border Name="Moved" Position="80,0" Size="10,10" RenderTranslate="5,5" RenderScale="2,2"/>
  </Canvas>
</UI>)XML", "the paint document parses");
    UIDocumentInstance Doc(Asset);
    Doc.Layout(UIVec2{ 100, 100 });
    Expect(Doc.Paint(), "the first paint draws");
    Expect(!Doc.Paint(), "nothing changed: no paint");

    const RenderCore::UIDrawList& List = Doc.GetDrawList();
    Expect(List.GetCommands().size() == 3, "unclipped rects batch into one command, the clipped child its own, then unclipped again");
    Expect(List.GetVertices().size() >= 4 && List.GetVertices()[0].Color == RenderCore::PackUIColor(0, 1, 0, 1),
           "ZOrder -1 paints first though it is written second");
    if(List.GetCommands().size() == 3)
    {
        Expect(List.GetCommands()[0].IndexCount == 12 && List.GetCommands()[0].Clip.IsNone(), "the first two rects share a command");
        const RenderCore::UIClipRect& Clip = List.GetCommands()[1].Clip;
        Expect(Clip.X == 0 && Clip.Y == 50 && Clip.W == 20 && Clip.H == 20, "ClipChildren clips to its pixels");
    }
    bool bFaded = false;
    for(const RenderCore::UIVertex& Each : List.GetVertices())
    {
        if(Each.Color == RenderCore::PackUIColor(1, 1, 1, 0.5f)) bFaded = true;
    }
    Expect(bFaded, "Opacity multiplies the tint's alpha");

    const UIWidget* Moved = Doc.FindWidget("Moved");
    const UIRect Pixels = Doc.GetPixelRect(Moved->GetIndex());
    Expect(Near(Pixels.X, 80 - 5 + 5) && Near(Pixels.W, 20), "RenderScale scales about the centre, RenderTranslate moves it");
    Expect(Near(Doc.GetRect(Moved->GetIndex()).W, 10), "render transforms leave layout alone");
}

// === Text =====================================================================

static void TestText()
{
    std::printf("[text]\n");
    UIFontCache Fonts;
    UIFont* Font = Fonts.Find("", 24);
    Expect(Font != nullptr, "the engine's font bakes at 24 px");
    if(!Font) return;
    Expect(Font->GetLineHeight() > 24.0f && Font->GetAscent() > 0.0f, "line height and ascent are sensible");
    Expect(Font->GetGlyph('A').bValid && Font->GetGlyph(0xE9).bValid && Font->GetGlyph('A').Advance > 0, "ASCII and Latin-1 are baked");
    Expect(&Font->GetGlyph(0x4E2D) == &Font->GetGlyph('?'), "a codepoint not baked draws as '?'");
    Expect(Font->GetAtlas().GetData()[(2 * Font->GetAtlas().GetWidth() + 2) * 4 + 3] == 255, "the atlas keeps an opaque white texel");
    Expect(Fonts.Find("", 24) == Font && Fonts.GetFontCount() == 1, "a size is baked once");
    Expect(Fonts.Find("Nope/Missing.ttf", 24) == nullptr, "a missing font is null");

    UITextLayout One, Two, Wrapped, Centred;
    UIFontCache::Layout(*Font, "Hello", 0.0f, EUIJustify::Left, 1.0f, One);
    UIFontCache::Layout(*Font, "Hello\nWorld!", 0.0f, EUIJustify::Left, 1.0f, Two);
    Expect(One.Quads.size() == 20 && One.Size.X > 40.0f && Near(One.Size.Y, Font->GetLineHeight()), "five glyphs, one line");
    Expect(Near(Two.Size.Y, 2.0f * Font->GetLineHeight()), "a newline makes a second line");
    UIFontCache::Layout(*Font, "one two three four five six", 100.0f, EUIJustify::Left, 1.0f, Wrapped);
    Expect(Wrapped.Size.X <= 100.0f && Wrapped.Size.Y > 2.0f * Font->GetLineHeight(), "wrapping keeps lines within the width");
    UIFontCache::Layout(*Font, "a\nwide line", 0.0f, EUIJustify::Center, 1.0f, Centred);
    Expect(!Centred.Quads.empty() && Centred.Quads[0].X > 10.0f, "Center justifies a short line within the widest");

    auto Asset = Parse(R"XML(<UI ReferenceSize="1920,1080">
  <Canvas>
    <Text Name="Label" AutoSize="true" Text="Score" FontSize="24"/>
  </Canvas>
</UI>)XML", "a text document parses");
    UIDocumentInstance Doc(Asset);
    Doc.SetFonts(&Fonts);
    Doc.Layout(UIVec2{ 1920, 1080 });
    const UIRect Small = RectOf(Doc, "Label");
    Expect(Small.W > 30.0f && Near(Small.H, Font->GetLineHeight()), "an AutoSize Text is its text's size");
    Doc.Layout(UIVec2{ 3840, 2160 });
    const UIRect Big = RectOf(Doc, "Label");
    Expect(Near(Big.W, Small.W, 2.0f) && Fonts.Find("", 48) != nullptr, "at 2x the font bakes at 48 px; the layout size barely moves");
    Doc.Paint();
    Expect(Doc.GetDrawList().GetVertices().size() == 20, "the label paints its glyphs");
}

// === Styles and files =========================================================

static const char* const MENU_XML = R"XML(<?xml version="1.0" encoding="utf-8"?>
<UI Format="1" ReferenceSize="1920,1080" ScaleMode="FitHeight" InputMode="UIOnly" OnBack="Native:Test.Back">
  <Styles>
    <Style Name="MenuButton" Type="Button" Size="420,72" Normal.Tint="#203040FF" Hovered.Tint="#FFE0A0FF"/>
    <Style Name="Big" Size="500,80"/>
    <Style Name="TextOnly" Type="Text" FontSize="40"/>
  </Styles>
  <Canvas Name="Root">
    <VerticalBox Name="Buttons" Anchors="0.5,0.5,0.5,0.5" Pivot="0.5,0.5" AutoSize="true">
      <Button Name="Play" Class="MenuButton" OnClick="Native:Test.Click($self)" InitialFocus="true">
        <Text Text="Play"/>
      </Button>
      <Button Name="Options" Class="MenuButton TextOnly Big" Normal.Tint="#FF0000FF" OnClick="Native:Test.Click($self)" IsEnabled="false"/>
      <Button Name="Quit" Class="MenuButton" OnClick="Native:Test.Click($self); Native:Test.Close"/>
    </VerticalBox>
  </Canvas>
  <Animations>
    <Animation Name="Intro" Duration="0.6">
      <Track Target="Play" Property="Opacity" Easing="CubicOut">
        <Key Time="0" Value="0"/>
        <Key Time="0.6" Value="1"/>
      </Track>
    </Animation>
  </Animations>
  <VisualStates Target="Play">
    <State Name="Hovered">
      <Set Property="RenderScale" Value="1.05,1.05"/>
    </State>
  </VisualStates>
</UI>
)XML";

static void TestStylesAndFiles()
{
    std::printf("[styles and files]\n");
    auto Asset = Parse(MENU_XML, "the menu document parses");
    Expect(Asset->Styles.size() == 3 && Asset->Widgets.size() == 6 && Asset->Reserved.size() == 2, "styles, widgets and reserved sections are read");
    Expect(Asset->Animations.size() == 1 && Asset->Animations[0].Tracks.size() == 1 && Asset->Animations[0].Tracks[0].Keys.size() == 2
           && Asset->Animations[0].Tracks[0].Easing == EUIEasing::CubicOut, "the reserved animation is read into its stub");

    UIDocumentInstance Doc(Asset);
    UIWidget* Options = Doc.FindWidget("Options");
    Expect(Options && Doc.GetProperty(*Options, "Size").AsVec2() == UIVec2{ 500, 80 }, "styles apply in Class order: Big after MenuButton");
    Expect(Options && Doc.GetProperty(*Options, "Normal.Tint").AsColor() == UIColor{ 1, 0, 0, 1 }, "the widget's own value beats its styles");
    Expect(Options && Doc.GetProperty(*Options, "Hovered.Tint").AsColor().R == 1.0f, "an unoverridden style value stays");
    Expect(Doc.GetFocused() == int32_t(Doc.FindWidget("Play")->GetIndex()), "InitialFocus focuses");

    /* Text round trip: byte-identical through a second write. */
    const std::string Written = WriteUIXml(*Asset);
    UIDocumentAsset Again;
    std::string Error;
    Expect(ParseUIXml(Written, Again, &Error), "the written XML reads back");
    Expect(WriteUIXml(Again) == Written, "a read and write changes nothing (deterministic)");
    Expect(Written.find("<Animations>") != std::string::npos && Written.find("<VisualStates Target=\"Play\">") != std::string::npos
           && Written.find("CubicOut") != std::string::npos, "the reserved sections are kept as written");

    /* The cooked form: styles applied, deterministic, and it reads back. */
    const std::vector<uint8_t> Cooked = WriteUIBinary(*Asset);
    Expect(DetectUIFileFormat(Cooked) == EUIFileFormat::Binary, "the cooked form is detected as binary");
    Expect(WriteUIBinary(*Asset) == Cooked, "cooking twice gives the same bytes");
    UIDocumentAsset FromCooked;
    Expect(ReadUIBinary(Cooked, FromCooked, &Error), "the cooked form reads back");
    Expect(FromCooked.Styles.empty() && FromCooked.Widgets.size() == 6 && FromCooked.Reserved.size() == 2
           && FromCooked.Settings.InputMode == EUIInputMode::UIOnly, "it has its widgets, settings and reserved sections, no styles");
    const std::string* Size = FromCooked.Widgets[size_t(FromCooked.FindWidget("Options"))].Find("Size");
    Expect(Size && *Size == "500,80", "a style's values are written into the widget");
    UIDocumentInstance CookedDoc(std::make_shared<UIDocumentAsset>(FromCooked));
    Expect(CookedDoc.IsValid() && Doc.GetProperty(*Doc.FindWidget("Quit"), "Size") == CookedDoc.GetProperty(*CookedDoc.FindWidget("Quit"), "Size"),
           "the cooked document instances as the text one did");
    std::vector<uint8_t> Damaged = Cooked;
    Damaged.resize(Damaged.size() / 2);
    Expect(!ReadUIBinary(Damaged, FromCooked, &Error), "a truncated cooked file is refused");

    /* On disk: save, cook, load through the file provider. */
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleUITests";
    std::filesystem::create_directories(Dir);
    const std::string Source = (Dir / "Menu.nmui").string(), Out = (Dir / "Cooked" / "Menu.nmui").string();
    Expect(SaveUIXml(Source, *Asset, &Error), "SaveUIXml writes");
    Expect(CookUIFile(Source, Out, &Error), "CookUIFile cooks it");
    EngineCore::FilesystemProvider Disk(Dir.string());
    EngineCore::IFileProvider* Previous = EngineCore::GetFileProvider();
    EngineCore::SetFileProvider(&Disk);
    UIDocumentAsset Loaded;
    Expect(LoadUIDocument("Cooked/Menu.nmui", Loaded, &Error) && Loaded.Widgets.size() == 6, "LoadUIDocument reads the cooked file");
    Expect(LoadUIDocument("Menu.nmui", Loaded, &Error) && Loaded.Styles.size() == 3, "and, in an editor build, the XML");
    EngineCore::SetFileProvider(Previous);
    std::filesystem::remove_all(Dir);

    /* Bad documents. */
    UIDocumentAsset Bad;
    Expect(!ParseUIXml("<UI><Canvas/><Canvas/></UI>", Bad, &Error), "two root widgets are refused");
    Expect(!ParseUIXml("<UI Wobble=\"1\"><Canvas/></UI>", Bad, &Error), "an unknown setting is refused");
    Expect(!ParseUIXml("<Menu/>", Bad, &Error), "a document without <UI> is refused");
    Expect(ParseUIXml("<UI><Image><Text/></Image></UI>", Bad, &Error) && !Bad.Validate(&Error), "an Image with a child fails validation");
    Expect(ParseUIXml("<UI><Blob/></UI>", Bad, &Error) && !Bad.Validate(&Error), "an unknown widget type fails validation");
    UIDocumentInstance Invalid(std::make_shared<UIDocumentAsset>(Bad));
    Expect(!Invalid.IsValid(), "an invalid document instances empty, logged");

    std::vector<UIWidgetDesc> Fragment;
    Expect(ParseUIXmlFragment(WriteUIXmlFragment(*Asset, Asset->FindWidget("Buttons")), Fragment, &Error)
           && Fragment.size() == 5 && Fragment[0].Type == "VerticalBox" && Fragment[1].Parent == 0 && Fragment[2].Parent == 1,
           "a subtree copies out and back");
}

// === Structure editing ========================================================

static void TestStructureEditing()
{
    std::printf("[structure editing]\n");
    UIDocumentAsset Asset;
    std::string Error;
    Expect(ParseUIXml(R"XML(<UI><Canvas Name="Root">
  <VerticalBox Name="Box"><Text Name="A"/><Text Name="B"/></VerticalBox>
  <Border Name="Panel"><Text Name="Label"/></Border>
  <Image Name="Pic"/>
</Canvas></UI>)XML", Asset, &Error), "the editing document parses");
    const auto NameAt = [&Asset](const int32_t InIndex) { return InIndex >= 0 ? Asset.Widgets[size_t(InIndex)].GetName() : std::string("-"); };
    const auto Order = [&Asset]()
    {
        std::string Out;
        for(const UIWidgetDesc& Each : Asset.Widgets) { Out += Each.GetName() + "(" + std::to_string(Each.Parent) + ") "; }
        return Out;
    };

    Expect(Asset.CanTakeChild(Asset.FindWidget("Box")) && !Asset.CanTakeChild(Asset.FindWidget("Panel"))
           && !Asset.CanTakeChild(Asset.FindWidget("Pic")), "room: a box any, a full Border none, an Image never");

    const std::vector<UIWidgetDesc> Copied = Asset.CopyWidgets(Asset.FindWidget("Box"));
    Expect(Copied.size() == 3 && Copied[0].Parent == -1 && Copied[1].Parent == 0 && Copied[2].Parent == 0, "a copy is its subtree, relative");

    /* Duplicate the box before Panel. */
    const int32_t Pasted = Asset.InsertWidgets(0, Asset.FindWidget("Panel"), Copied);
    Expect(Pasted == 4 && Asset.Validate(&Error) && NameAt(5) == "A" && Asset.Widgets[5].Parent == 4,
           "inserted before a sibling, its children follow and point at it");
    Expect(Asset.Widgets[size_t(Asset.FindWidget("Label"))].Parent == Asset.FindWidget("Panel"), "widgets after it keep their parents");

    /* Move B into the duplicate, first. */
    const int32_t Moved = Asset.MoveWidget(Asset.FindWidget("B"), Pasted, Pasted + 1);
    Expect(Moved == Pasted && Asset.Widgets[size_t(Moved)].Parent == Pasted - 1 && Asset.Validate(&Error),
           "a move into the duplicate, before its first child, keeps the document valid");
    Expect(Asset.InsertWidgets(Asset.FindWidget("Panel"), -1, { UIWidgetDesc{ "Text", -1, {} } }) == -1, "a full Border refuses another child");
    Expect(Asset.MoveWidget(Asset.FindWidget("Box"), Asset.FindWidget("A"), -1) == -1, "a widget cannot move into its own subtree");
    Expect(Asset.MoveWidget(0, 1, -1) == -1, "the root does not move");

    const size_t Before = Asset.Widgets.size();
    Expect(Asset.RemoveWidget(Asset.FindWidget("Panel")) && Asset.Widgets.size() == Before - 2 && Asset.Validate(&Error),
           "removing takes the subtree and keeps the rest valid");
    Expect(!Asset.RemoveWidget(0), "the root stays");

    /* Reorder within a box: last to first. */
    Asset = UIDocumentAsset();
    ParseUIXml(R"XML(<UI><VerticalBox><Text Name="One"/><Text Name="Two"/><Text Name="Three"/></VerticalBox></UI>)XML", Asset, &Error);
    Expect(Asset.MoveWidget(3, 0, 1) == 1 && NameAt(1) == "Three" && NameAt(2) == "One" && NameAt(3) == "Two", "reordered within a parent");
    Expect(Asset.MoveWidget(1, 0, -1) == 3 && NameAt(3) == "Three", "and back to last");
    (void)Order;

    /* A draw list appended at an offset moves with its clips. */
    RenderCore::UIDrawList Inner, Outer;
    Inner.PushClip({ 0, 0, 10, 10 });
    Inner.AddRect({ 0, 0, 5, 5 }, 0xFFFFFFFFu);
    Outer.Append(Inner, 100.0f, 50.0f);
    Expect(Outer.GetVertices()[0].X == 100.0f && Outer.GetVertices()[0].Y == 50.0f && Outer.GetCommands()[0].Clip.X == 100,
           "Append's offset moves vertices and clips");
}

// === Actions, bindings and input ==============================================

static int         GClicks = 0;
static std::string GLastClicked;
static UISystem*   GCloseFrom = nullptr;

static void ClickAction(const UIActionContext& InContext)
{
    ++GClicks;
    GLastClicked = InContext.Args->Count > 0 ? InContext.Args->Args[0].String : "";
}

static void CloseAction(const UIActionContext& InContext)
{
    /* Closing the document whose button fired this, from inside the action. */
    if(InContext.System && InContext.Document) { InContext.System->Close(InContext.Document->GetPath()); }
}

static int GBacks = 0;
static void BackAction(const UIActionContext&) { ++GBacks; }

static int    GValues = 0;
static double GLastValue = -1.0;
static void ValueAction(const UIActionContext& InContext)
{
    ++GValues;
    GLastValue = InContext.Args->Count > 0 ? InContext.Args->Args[0].Number : -1.0;
}

UI_DEFINE_ACTIONS(Test)
{
    Actions.Add("Value", &ValueAction, "Records its number");
    Actions.Add("Click", &ClickAction, "Counts a click");
    Actions.Add("Close", &CloseAction);
    Actions.Add("Back", &BackAction);
}

static UIInputFrame Pointer(const UIVec2& InAt, const bool bInPress, const bool bInRelease)
{
    UIInputFrame Frame;
    Frame.MousePixels      = InAt;
    Frame.MousePressed[0]  = bInPress;
    Frame.MouseDown[0]     = bInPress;
    Frame.MouseReleased[0] = bInRelease;
    return Frame;
}

static UIVec2 CentreOf(const UIDocumentInstance& InDoc, const char* InName)
{
    const UIRect R = InDoc.GetPixelRect(InDoc.FindWidget(InName)->GetIndex());
    return UIVec2{ R.X + R.W * 0.5f, R.Y + R.H * 0.5f };
}

// === Switcher =================================================================

static void TestSwitcher()
{
    std::printf("[switcher]\n");
    UISystem System;
    const UIDocumentHandle Handle = System.OpenAsset(Parse(R"XML(<UI InputMode="UIOnly" ReferenceSize="1000,1000" ScaleMode="None">
  <Canvas>
    <Button Name="Tab" Position="0,0" Size="100,50" OnClick="Native:UI.ShowPage('Pages', 'Second')"/>
    <Switcher Name="Pages" Position="0,100" AutoSize="true" ActiveIndex="{Bind:Page}">
      <VerticalBox Name="First"><Button Name="A" Size="10,10"><Spacer Size="300,40"/></Button></VerticalBox>
      <VerticalBox Name="Second"><Button Name="B"><Spacer Size="80,200"/></Button></VerticalBox>
      <Spacer Name="Third" Size="10,10"/>
    </Switcher>
  </Canvas>
</UI>)XML", "a switcher document parses"), "Test/Switcher.nmui");
    UIDocumentInstance& Doc = *System.Find(Handle);
    System.Update(UIVec2{ 1000, 1000 });
    auto* Pages = dynamic_cast<UISwitcher*>(Doc.FindWidget("Pages"));
    Expect(Pages && Pages->GetPageCount() == 3, "a Switcher's pages are its children");
    if(!Pages) return;

    const UIRect R = RectOf(Doc, "Pages");
    Expect(Near(R.W, 300 + 32) && Near(R.H, 200 + 16), "it is the size of its largest page (each axis)");
    const uint32_t A = Doc.FindWidget("A")->GetIndex(), B = Doc.FindWidget("B")->GetIndex();
    Expect(Doc.IsShown(A) && !Doc.IsShown(B), "only the active page is shown");
    Expect(Doc.HitTest(UIVec2{ 50, 120 }) == int32_t(A), "the active page is hit");
    Expect(Doc.FindNextFocus(+1) != int32_t(B), "a hidden page's widgets take no focus");

    /* Focus on page one, then switch: focus leaves the hidden page. */
    Doc.SetFocus(int32_t(A));
    System.SetValue("Page", UIValue::Number(1));
    System.Tick(0.016f);
    System.Update(UIVec2{ 1000, 1000 });
    Expect(Pages->ActiveIndex == 1 && Doc.IsShown(B) && !Doc.IsShown(A), "a bound ActiveIndex follows the data store");
    Expect(Doc.GetFocused() != int32_t(A), "focus does not stay on a page that went away");
    Expect(Doc.HitTest(UIVec2{ 50, 120 }) == int32_t(B), "the new page takes the hits");
    Expect(RectOf(Doc, "Pages").W == R.W, "switching pages does not move it");

    Expect(Pages->ShowPage("2") && Pages->ActiveIndex == 2, "ShowPage by number");
    Expect(Pages->ShowPage("First") && Pages->ActiveIndex == 0, "ShowPage by name");
    Expect(!Pages->ShowPage("Fourth") && !Pages->ShowPage("7") && Pages->ActiveIndex == 0, "an unknown page changes nothing");

    /* The native action, from a click. */
    const UIVec2 Tab = CentreOf(Doc, "Tab");
    System.ProcessInput(Pointer(Tab, true, false));
    System.ProcessInput(Pointer(Tab, false, true));
    System.RunQueuedActions();
    Expect(Pages->ActiveIndex == 1, "Native:UI.ShowPage('Pages', 'Second') switches it");

    Pages->ActiveIndex = 9;
    Expect(Pages->GetActiveChild() == int32_t(Doc.FindWidget("Third")->GetIndex()), "an index past the last shows the last");
}

// === CheckBox and Slider ======================================================

static void TestInputWidgets()
{
    std::printf("[check box and slider]\n");
    UISystem System;
    System.SetValue("Opt.Shadows", UIValue::Bool(true));
    System.SetValue("Opt.Speed", UIValue::Number(1.0));
    const UIDocumentHandle Handle = System.OpenAsset(Parse(R"XML(<UI InputMode="UIOnly" ReferenceSize="1000,1000" ScaleMode="None">
  <Canvas>
    <VerticalBox Position="100,100" AutoSize="true" Spacing="10">
      <CheckBox Name="Shadows" Checked="{Bind:Opt.Shadows}" OnChanged="Native:Test.Value($value)" InitialFocus="true">
        <Text Name="ShadowsLabel" Text="Shadows" FontSize="20"/>
      </CheckBox>
      <Slider Name="Speed" Size="400,40" ThumbSize="20,40" Min="0.25" Max="3" Step="0.05" Value="{Bind:Opt.Speed}" OnChanged="Native:Test.Value($value)"/>
      <Slider Name="Free" Size="200,20" ThumbSize="0,20"/>
    </VerticalBox>
    <Text Name="SpeedText" Position="0,500" AutoSize="true" Text="x{Bind:Opt.Speed}"/>
  </Canvas>
</UI>)XML", "a settings document parses"), "Test/Settings.nmui");
    UIDocumentInstance& Doc = *System.Find(Handle);
    System.Tick(0.016f);
    System.Update(UIVec2{ 1000, 1000 });
    auto* Check  = dynamic_cast<UICheckBox*>(Doc.FindWidget("Shadows"));
    auto* Slider = dynamic_cast<UISlider*>(Doc.FindWidget("Speed"));
    auto* Free   = dynamic_cast<UISlider*>(Doc.FindWidget("Free"));
    Expect(Check && Slider && Free, "CheckBox and Slider are made from XML");
    if(!Check || !Slider || !Free) return;
    Expect(Check->bChecked && Near(Slider->Value, 1.0f), "both show the store's values");

    /* The check box: its row is the box, the gap and the label. */
    const UIRect Row = RectOf(Doc, "Shadows"), Label = RectOf(Doc, "ShadowsLabel");
    Expect(Near(Label.X, Row.X + 32 + 16) && Row.H >= 32, "its label sits right of the box");

    GValues = 0;
    const UIVec2 OnLabel = CentreOf(Doc, "ShadowsLabel");
    System.ProcessInput(Pointer(OnLabel, true, false));
    System.ProcessInput(Pointer(OnLabel, false, true));
    System.RunQueuedActions();
    const UIValue* Shadows = System.GetValue("Opt.Shadows");
    Expect(!Check->bChecked && Shadows && Shadows->Type == EUIValueType::Bool && !Shadows->B, "a click on its label unticks it, and writes the store");
    Expect(GValues == 1 && GLastValue == 0.0, "OnChanged fires with $value 0");

    UIInputFrame Accept;
    Accept.bAccept = true;
    System.ProcessInput(Accept);
    System.RunQueuedActions();
    Expect(Check->bChecked && System.GetValue("Opt.Shadows")->B && GLastValue == 1.0, "Accept ticks the focused box back, $value 1");

    System.SetValue("Opt.Shadows", UIValue::Bool(false));
    System.Tick(0.016f);
    Expect(!Check->bChecked, "and the store still drives it: the binding is two-way");

    /* The slider: travel is its width less a thumb; a press jumps there. */
    System.Update(UIVec2{ 1000, 1000 });
    const UIRect S = Doc.GetPixelRect(Slider->GetIndex());
    const auto At = [&S](const float InT) { return UIVec2{ S.X + 10.0f + (S.W - 20.0f) * InT, S.Y + S.H * 0.5f }; };
    GValues = 0;
    System.ProcessInput(Pointer(At(1.0f), true, false));
    System.RunQueuedActions();
    Expect(Near(Slider->Value, 3.0f) && GValues == 1 && Near(float(GLastValue), 3.0f), "a press at its end jumps to Max");

    UIInputFrame Drag = Pointer(At(0.5f), false, false);
    Drag.MouseDown[0] = true;
    System.ProcessInput(Drag);
    System.RunQueuedActions();
    Expect(Near(Slider->Value, 1.6f, 0.001f) && GValues == 2, "dragging moves it, snapped to Step (0.25 + 27 * 0.05)");
    const UIValue* Speed = System.GetValue("Opt.Speed");
    Expect(Speed && Speed->AsString() == "1.6", "the store holds the snapped value, not a float's noise");

    Drag.MousePixels = UIVec2{ S.X - 300.0f, S.Y + 400.0f };
    System.ProcessInput(Drag);
    System.RunQueuedActions();
    Expect(Near(Slider->Value, 0.25f), "the drag holds outside it, clamped at Min");
    const int Before = GValues;
    System.ProcessInput(Drag);
    System.RunQueuedActions();
    Expect(GValues == Before, "a still pointer changes nothing");
    System.ProcessInput(Pointer(Drag.MousePixels, false, true));
    System.RunQueuedActions();
    Expect(Doc.GetPressed() < 0, "release lets go");

    System.Tick(0.016f);
    System.Update(UIVec2{ 1000, 1000 });
    const UIText* SpeedText = dynamic_cast<UIText*>(Doc.FindWidget("SpeedText"));
    Expect(SpeedText && SpeedText->Text == "x0.25", "a label bound to the same key follows the slider");

    /* Keys: left and right step it while focused; up and down still move focus. */
    Doc.SetFocus(int32_t(Slider->GetIndex()));
    UIInputFrame Right;
    Right.bNavRight = true;
    System.ProcessInput(Right);
    System.ProcessInput(Right);
    System.RunQueuedActions();
    Expect(Near(Slider->Value, 0.35f) && Doc.GetFocused() == int32_t(Slider->GetIndex()), "Right steps it twice, focus stays");
    UIInputFrame Left;
    Left.bNavLeft = true;
    for(int Each = 0; Each < 10; ++Each) { System.ProcessInput(Left); }
    Expect(Near(Slider->Value, 0.25f) && Doc.GetFocused() == int32_t(Slider->GetIndex()), "Left stops at Min and keeps focus");
    UIInputFrame Down;
    Down.bNavDown = true;
    System.ProcessInput(Down);
    Expect(Doc.GetFocused() == int32_t(Free->GetIndex()), "Down moves focus to the next slider");
    System.ProcessInput(Right);
    Expect(Near(Free->Value, 0.55f), "with no Step, a twentieth of the range");

    Expect(Near(Slider->Snap(10.0), 3.0f) && Near(Slider->Snap(-1.0), 0.25f) && Near(Slider->Snap(1.27), 1.25f), "Snap clamps and rounds to Step");
    Slider->bEnabled = false;
    const float Held = Slider->Value;
    Doc.SetPressed(-1);
    System.ProcessInput(Pointer(At(1.0f), true, false));
    Expect(Slider->Value == Held, "a disabled slider ignores the pointer");
}

// === ScrollBox ================================================================

static void TestScrollBox()
{
    std::printf("[scroll box]\n");
    std::string Rows;
    for(int Row = 0; Row < 10; ++Row)
    {
        Rows += "<Button Name=\"Row" + std::to_string(Row) + "\" Padding=\"0,0\"><Spacer Size=\"180,40\"/></Button>";
    }
    UISystem System;
    const UIDocumentHandle Handle = System.OpenAsset(Parse(R"XML(<UI InputMode="UIOnly" ScaleMode="None">
  <Canvas>
    <ScrollBox Name="List" Position="0,0" Size="200,100" ScrollOffset="{Bind:List.Scroll}">
      <VerticalBox Name="Rows">)XML" + Rows + R"XML(</VerticalBox>
    </ScrollBox>
    <ScrollBox Name="Outer" Position="400,0" Size="200,100">
      <VerticalBox>
        <ScrollBox Name="Inner" Size="180,50"><Spacer Size="100,100"/></ScrollBox>
        <Spacer Size="100,200"/>
      </VerticalBox>
    </ScrollBox>
  </Canvas>
</UI>)XML", "a scroll box document parses"), "Test/Scroll.nmui");
    UIDocumentInstance& Doc = *System.Find(Handle);
    const UIVec2 View{ 1000, 1000 };
    System.Update(View);
    auto* List = dynamic_cast<UIScrollBox*>(Doc.FindWidget("List"));
    Expect(List && Near(List->GetScroll().ContentLength, 400.0f) && Near(List->GetScroll().MaxOffset(), 300.0f),
           "content is its child at full length; the view is its own size");
    if(!List) return;

    UIInputFrame Wheel = Pointer(UIVec2{ 50, 50 }, false, false);
    Wheel.Wheel = -1.0f;
    System.ProcessInput(Wheel);
    System.Update(View);
    Expect(Near(List->ScrollOffset, 60.0f), "a wheel notch down scrolls one ScrollStep");
    const UIValue* Bound = System.GetValue("List.Scroll");
    Expect(Bound && Near(float(Bound->AsNumber()), 60.0f), "ScrollOffset writes back to its key");
    Wheel.Wheel = -10.0f;
    System.ProcessInput(Wheel);
    System.Update(View);
    Expect(Near(List->ScrollOffset, 300.0f), "and stops at the end");

    const int32_t Hit = Doc.HitTest(UIVec2{ 50, 10 });
    Expect(Hit == int32_t(Doc.FindWidget("Row7")->GetIndex()) || Doc.FindInteractive(Hit) == int32_t(Doc.FindWidget("Row7")->GetIndex()),
           "what is hit is what is shown: row 7 at the top, scrolled 300");
    const UIRect Row0 = Doc.GetPixelRect(Doc.FindWidget("Row0")->GetIndex());
    Expect(Near(Row0.Y, -300.0f) && Doc.FindInteractive(Doc.HitTest(UIVec2{ 50, 2 })) != int32_t(Doc.FindWidget("Row0")->GetIndex()),
           "a row scrolled out is drawn above the view and not hit (clipped)");

    Doc.SetFocus(int32_t(Doc.FindWidget("Row0")->GetIndex()));
    System.Update(View);
    Expect(Near(List->ScrollOffset, 0.0f), "focusing a row above the view scrolls back to it");
    Doc.SetFocus(int32_t(Doc.FindWidget("Row9")->GetIndex()));
    System.Update(View);
    Expect(Near(List->ScrollOffset, 300.0f), "and one below scrolls just far enough to show it");
    UIInputFrame Up;
    Up.bNavUp = true;
    for(int Each = 0; Each < 4; ++Each) { System.ProcessInput(Up); System.Update(View); }
    Expect(Doc.GetFocused() == int32_t(Doc.FindWidget("Row5")->GetIndex()) && Near(List->ScrollOffset, 200.0f),
           "arrow keys move focus row by row, the view following");

    /* The bar: drag the thumb, click the track for a page. */
    List->ScrollTo(0.0f);
    System.Update(View);
    System.ProcessInput(Pointer(UIVec2{ 196, 5 }, true, false));
    UIInputFrame Drag = Pointer(UIVec2{ 196, 80 }, false, false);
    Drag.MouseDown[0] = true;
    System.ProcessInput(Drag);
    System.ProcessInput(Pointer(UIVec2{ 196, 80 }, false, true));
    System.Update(View);
    Expect(Near(List->ScrollOffset, 300.0f, 1.0f), "dragging the thumb to the bottom scrolls to the end");
    System.ProcessInput(Pointer(UIVec2{ 196, 5 }, true, false));
    System.ProcessInput(Pointer(UIVec2{ 196, 5 }, false, true));
    System.Update(View);
    Expect(Near(List->ScrollOffset, 200.0f, 1.0f), "a click on the track above the thumb goes back a page");

    /* Nested: the inner box first; at its end, the outer. */
    auto* Outer = dynamic_cast<UIScrollBox*>(Doc.FindWidget("Outer"));
    auto* Inner = dynamic_cast<UIScrollBox*>(Doc.FindWidget("Inner"));
    UIInputFrame Nested = Pointer(UIVec2{ 450, 20 }, false, false);
    Nested.Wheel = -1.0f;
    System.ProcessInput(Nested);
    System.Update(View);
    Expect(Inner && Outer && Near(Inner->ScrollOffset, 50.0f) && Near(Outer->ScrollOffset, 0.0f), "the wheel scrolls the inner box first");
    System.ProcessInput(Nested);
    System.Update(View);
    Expect(Outer && Near(Outer->ScrollOffset, 60.0f), "at its end, the outer one");
}

static void TestInputAndActions()
{
    std::printf("[input and actions]\n");
    Expect(FindUIAction("Test.Click") != nullptr, "UI_DEFINE_ACTIONS registers Group.Name");

    UISystem System;
    const UIVec2 Viewport{ 1920, 1080 };
    const UIDocumentHandle Menu = System.OpenAsset(Parse(MENU_XML, "menu"), "Test/Menu.nmui");
    Expect(Menu != UI_DOCUMENT_INVALID && System.IsOpen("Test/Menu.nmui"), "a document opens");
    System.Update(Viewport);
    UIDocumentInstance& Doc = *System.Find(Menu);

    /* A click: press and release on the same button. */
    GClicks = 0;
    const UIVec2 Play = CentreOf(Doc, "Play");
    UIInputResult Result = System.ProcessInput(Pointer(Play, true, false));
    Expect(Result.bConsumeMouse && Result.bShowCursor && Result.bOverWidget, "a UIOnly menu takes the mouse and shows the cursor");
    Expect(Doc.GetPressed() == int32_t(Doc.FindWidget("Play")->GetIndex()), "the press is held by the button");
    System.RunQueuedActions();
    Expect(GClicks == 0, "a press alone does not click");
    System.ProcessInput(Pointer(Play, false, true));
    System.RunQueuedActions();
    Expect(GClicks == 1 && GLastClicked == "Play", "the release on it clicks, $self naming it");

    /* Pressed on one, released on another: no click. */
    System.ProcessInput(Pointer(Play, true, false));
    System.ProcessInput(Pointer(CentreOf(Doc, "Quit"), false, true));
    System.RunQueuedActions();
    Expect(GClicks == 1, "a release elsewhere is no click");

    /* A disabled button takes no click. */
    const UIVec2 Options = CentreOf(Doc, "Options");
    System.ProcessInput(Pointer(Options, true, false));
    System.ProcessInput(Pointer(Options, false, true));
    System.RunQueuedActions();
    Expect(GClicks == 1, "a disabled button does not click");

    /* Navigation: focus starts on Play; down skips the disabled Options. */
    UIInputFrame Nav;
    Nav.bHasMouse = false;
    Nav.bNavDown = true;
    Result = System.ProcessInput(Nav);
    Expect(Doc.GetFocused() == int32_t(Doc.FindWidget("Quit")->GetIndex()), "Down moves focus to the next enabled button");
    Expect(Result.bConsumeKeyboard && Result.bConsumeGamepad, "navigation is consumed");
    Nav = UIInputFrame();
    Nav.bHasMouse = false;
    Nav.bNavUp = true;
    System.ProcessInput(Nav);
    Expect(Doc.GetFocused() == int32_t(Doc.FindWidget("Play")->GetIndex()), "Up moves back");
    Nav = UIInputFrame();
    Nav.bHasMouse = false;
    Nav.bAccept = true;
    System.ProcessInput(Nav);
    System.RunQueuedActions();
    Expect(GClicks == 2, "Accept clicks the focused button");
    GBacks = 0;
    Nav = UIInputFrame();
    Nav.bHasMouse = false;
    Nav.bBack = true;
    System.ProcessInput(Nav);
    System.RunQueuedActions();
    Expect(GBacks == 1, "Back runs the document's OnBack");

    /* Closing the document from its own button's action is safe. */
    System.Update(Viewport);
    const UIVec2 Quit = CentreOf(Doc, "Quit");
    System.ProcessInput(Pointer(Quit, true, false));
    System.ProcessInput(Pointer(Quit, false, true));
    System.RunQueuedActions();
    Expect(!System.IsOpen("Test/Menu.nmui") && GClicks == 3, "a button's actions run in order, then its document closes");

    /* A HUD (GameAndUI): clicks pass through where there is nothing to hit. */
    auto Hud = Parse(R"XML(<UI InputMode="GameAndUI">
  <Canvas Name="Root">
    <Border Name="Panel" Position="0,0" Size="200,100"/>
    <Image Name="Crosshair" Anchors="0.5,0.5,0.5,0.5" Size="32,32" Pivot="0.5,0.5" Visibility="HitTestInvisible"/>
    <Text Name="Score" Position="300,0" AutoSize="true" Text="Score: {Bind:Player0.Score}"/>
    <ProgressBar Name="Health" Position="0,200" Size="300,20" Value="{Bind:Player0.Health}"/>
  </Canvas>
</UI>)XML", "the HUD parses");
    const UIDocumentHandle HudHandle = System.OpenAsset(Hud, "Test/HUD.nmui");
    System.Update(Viewport);
    Result = System.ProcessInput(Pointer(UIVec2{ 1000, 700 }, true, false));
    Expect(!Result.bConsumeMouse && !Result.bShowCursor, "outside its widgets a HUD lets the click through");
    Result = System.ProcessInput(Pointer(UIVec2{ 960, 540 }, true, false));
    Expect(!Result.bConsumeMouse, "a HitTestInvisible crosshair is clicked through");
    Result = System.ProcessInput(Pointer(UIVec2{ 50, 50 }, true, false));
    Expect(Result.bConsumeMouse, "over its panel, the HUD takes the click");
    Result = System.ProcessInput(UIInputFrame());
    Expect(!Result.bConsumeKeyboard, "a HUD without focus leaves the keyboard to the game");

    /* Bindings: interpolated text and a whole value. */
    UIDocumentInstance& HudDoc = *System.Find(HudHandle);
    UIWidget* Score = HudDoc.FindWidget("Score");
    Expect(HudDoc.GetProperty(*Score, "Text").AsString() == "Score: ", "before a value is set, the binding shows empty");
    System.SetValue("Player0.Score", UIValue::Number(42));
    System.SetValue("Player0.Health", UIValue::Number(0.25));
    System.Tick(0.016f);
    Expect(HudDoc.GetProperty(*Score, "Text").AsString() == "Score: 42", "Text follows the data store");
    Expect(Near(float(HudDoc.GetProperty(*HudDoc.FindWidget("Health"), "Value").AsNumber()), 0.25f), "a whole-value binding sets a number");
    System.Update(Viewport);
    const uint64_t Submits = System.GetSubmitCount();
    System.SetValue("Player0.Score", UIValue::Number(42));
    System.Tick(0.016f);
    Expect(!HudDoc.IsPaintDirty() && !HudDoc.IsLayoutDirty(), "setting the same value changes nothing");
    HudDoc.SetPropertyText(*Score, "Text", "fixed");
    System.SetValue("Player0.Score", UIValue::Number(7));
    System.Tick(0.016f);
    Expect(HudDoc.GetProperty(*Score, "Text").AsString() == "fixed", "a plain value replaces a binding");
    Expect(System.GetSubmitCount() == Submits, "without a renderer nothing is submitted");

    Expect(!HudDoc.PlayAnimation("Intro"), "PlayAnimation is reserved: false");
    System.CloseAll(true);
    Expect(System.GetDocumentCount() == 0, "CloseAll closes everything");
}

/* Lua and Event actions, through a real script engine and World. */
static void TestScriptedActions()
{
    std::printf("[scripted actions]\n");
    GameCore::World World;
    GameCore::ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "a script engine for Lua actions");
    Scripts.BindWorld(&World);
    Expect(Scripts.RunString("Menu = { Clicks = 0 }\n"
                             "function Menu.Play(Who, N) Menu.Clicks = Menu.Clicks + N; Menu.Who = Who end\n"
                             "Seen = 0\n"
                             "Events.On('Menu.Changed', function(Name) Seen = Seen + 1; SeenName = Name end)\n", "menu-lua"),
           "the Lua side loads");

    UISystem System;
    System.SetScriptEngine(&Scripts);
    System.SetWorld(&World);
    SetScriptUISystem(&System);
    auto Asset = Parse(R"XML(<UI InputMode="UIOnly">
  <Canvas>
    <Button Name="Go" Size="100,50" OnClick="Lua:Menu.Play($self, 2); Event:Menu.Changed($self)"/>
  </Canvas>
</UI>)XML", "a scripted document parses");
    const UIDocumentHandle Handle = System.OpenAsset(Asset, "Test/Scripted.nmui");
    System.Update(UIVec2{ 1920, 1080 });
    const UIVec2 Go = CentreOf(*System.Find(Handle), "Go");
    System.ProcessInput(Pointer(Go, true, false));
    System.ProcessInput(Pointer(Go, false, true));
    System.RunQueuedActions();
    World.Tick(0.016f);
    Expect(Scripts.RunString("assert(Menu.Clicks == 2 and Menu.Who == 'Go', tostring(Menu.Who))", "assert-lua"), "Lua: called with its arguments");
    Expect(Scripts.RunString("assert(Seen == 1 and SeenName == 'Go')", "assert-event"), "Event: emitted on the World's bus");

    /* The UI Lua library acts on the same system. */
    Expect(Scripts.RunString("UI.SetNumber('Ammo', 12)\n"
                             "assert(UI.GetNumber('Ammo') == 12)\n"
                             "assert(UI.IsOpen('Test/Scripted.nmui'))\n"
                             "assert(UI.SetProperty('Test/Scripted.nmui', 'Go', 'Opacity', '0.5'))\n"
                             "assert(not UI.PlayAnimation('Test/Scripted.nmui', 'Intro'))\n"
                             "assert(UI.Close('Test/Scripted.nmui'))\n", "ui-library"), "the Lua UI library works");
    Expect(System.GetValue("Ammo") && System.GetValue("Ammo")->AsNumber() == 12.0 && !System.IsOpen("Test/Scripted.nmui"),
           "its calls land on the system");

    System.SetExecuteActions(false);
    UIActionList Logged;
    Logged.Parse("Native:Test.Click('x')");
    const int Before = GClicks;
    System.QueueActions(Logged, nullptr, nullptr);
    System.RunQueuedActions();
    Expect(GClicks == Before && !System.GetActionLog().empty() && System.GetActionLog().back() == "Native:Test.Click",
           "with actions off (the editor's preview) they are logged, not run");

    SetScriptUISystem(nullptr);
    Scripts.UnbindWorld();
    Scripts.Shutdown();
}

// === UI graphs =============================================================

static void TestUIGraphs()
{
    std::printf("[ui graphs]\n");
    UIActionList Parsed;
    Expect(Parsed.Parse("Graph:Go_OnClick; Native:UI.Close") && Parsed.Actions.size() == 2
           && Parsed.Actions[0].Kind == EUIActionKind::Graph && Parsed.Actions[0].Target == "Go_OnClick", "Graph: parses, and chains");
    Expect(!Parsed.Parse("Graph:Menu.Go") && !Parsed.Parse("Graph:Go(1)") && Parsed.Parse("Graph:Go()"),
           "a Graph: handler is one name with no arguments");

    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleUIGraph";
    std::filesystem::create_directories(Dir);
    EngineCore::FilesystemProvider Disk(Dir.string());
    EngineCore::IFileProvider* Previous = EngineCore::GetFileProvider();
    EngineCore::SetFileProvider(&Disk);
    const auto WriteGraph = [&Dir](const char* InLua)
    {
        std::ofstream File(Dir / "Menu.ui.nmgraph", std::ios::binary | std::ios::trunc);
        File << "<?xml version=\"1.0\"?>\n<NodeGraph Version=\"1\" Schema=\"UI\">\n<gen_code>\n<Output Extension=\".lua\"><![CDATA["
             << InLua << "]]></Output>\n</gen_code>\n</NodeGraph>\n";
    };
    WriteGraph("return {\n"
               "  Clicks = 0,\n"
               "  OnOpen = function(self) Opened = (Opened or 0) + 1; LastOpened = self.Document end,\n"
               "  OnClose = function(self) Closed = (Closed or 0) + 1 end,\n"
               "  OnTick = function(self, dt) Ticks = (Ticks or 0) + 1 end,\n"
               "  Go_OnClick = function(self, Value, Sender) self.Clicks = self.Clicks + 1; Last = { self.Document, self.Clicks, Value, Sender } end,\n"
               "  Shut_OnClick = function(self) UI.Close(self.Document) end,\n"
               "}\n");
    {
        std::ofstream File(Dir / "Menu.nmui", std::ios::binary | std::ios::trunc);
        File << R"XML(<UI InputMode="UIOnly" ScaleMode="None" Graph="Menu.ui.nmgraph">
  <Canvas>
    <Button Name="Go" Position="0,0" Size="100,50" OnClick="Graph:Go_OnClick"/>
    <Button Name="Shut" Position="0,100" Size="100,50" OnClick="Graph:Shut_OnClick"/>
    <Button Name="Lost" Position="0,200" Size="100,50" OnClick="Graph:Missing"/>
  </Canvas>
</UI>)XML";
    }

    GameCore::World World;
    GameCore::ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "a script engine for UI graphs");
    Scripts.BindWorld(&World);
    UISystem System;
    System.SetScriptEngine(&Scripts);
    SetScriptUISystem(&System);

    const UIDocumentHandle A = System.Open("Menu.nmui");
    const UIDocumentHandle B = System.OpenAsset(UIAssetCache::Find("Menu.nmui"), "Other.nmui");
    Expect(A != UI_DOCUMENT_INVALID && B != UI_DOCUMENT_INVALID, "two documents with one UI graph");
    Expect(Scripts.RunString("assert(Opened == nil)", "not-yet"), "OnOpen waits for the action queue");
    System.RunQueuedActions();
    Expect(Scripts.RunString("assert(Opened == 2)", "opened"), "OnOpen runs once for each");

    System.Update(UIVec2{ 1000, 1000 });
    const auto Click = [&System](const UIDocumentHandle InDoc, const char* InWidget)
    {
        const UIVec2 At = CentreOf(*System.Find(InDoc), InWidget);
        System.ProcessInput(Pointer(At, true, false));
        System.ProcessInput(Pointer(At, false, true));
        System.RunQueuedActions();
    };
    /* B is over A (opened later, same layer, UIOnly): it takes the clicks. */
    Click(B, "Go");
    Click(B, "Go");
    Expect(Scripts.RunString("assert(Last[1] == 'Other.nmui' and Last[2] == 2 and Last[3] == 0 and Last[4] == 'Go', tostring(Last[2]))", "handler"),
           "Graph: calls the document's own instance with the value and the sender");
    const int Warnings = GWarnings;
    Click(B, "Lost");
    Expect(GWarnings == Warnings + 1, "a handler the graph lacks is warned about");

    System.Tick(0.016f);
    Expect(Scripts.RunString("assert(Ticks == 2)", "ticks"), "OnTick runs for each document whose logic has one");

    Click(B, "Shut");
    Expect(!System.IsOpen("Other.nmui") && System.IsOpen("Menu.nmui") && Scripts.RunString("assert(Closed == 1)", "closed"),
           "a handler may close its own document; OnClose runs once");

    Click(A, "Go");
    Expect(Scripts.RunString("assert(Last[1] == 'Menu.nmui' and Last[2] == 1)", "state"), "each document's self is its own");

    /* Saved again: the document rebuilds and its logic is read again. */
    WriteGraph("return { OnOpen = function(self) Reopened = true end, Go_OnClick = function(self) Last = 'new' end }\n");
    UIAssetCache::Invalidate("Menu.nmui");
    System.Tick(0.016f);
    System.RunQueuedActions();
    System.Update(UIVec2{ 1000, 1000 });
    Click(A, "Go");
    Expect(Scripts.RunString("assert(Closed == 2 and Reopened and Last == 'new')", "reload"),
           "a reload closes the old logic and opens the new");

    System.SetExecuteActions(false);
    Click(A, "Go");
    Expect(System.GetActionLog().back() == "Graph:Go_OnClick" && Scripts.RunString("assert(Last == 'new')", "logged"),
           "in the preview Graph: is logged, not run");
    System.SetExecuteActions(true);

    System.SetScriptEngine(nullptr);   // let go before the engine closes
    SetScriptUISystem(nullptr);
    Scripts.UnbindWorld();
    Scripts.Shutdown();
    EngineCore::SetFileProvider(Previous);
    std::filesystem::remove_all(Dir);
}

/* LightLab's pause menu as the game runs it: its document and its logic
 * graph (Content/UI/PauseMenu.ui.nmgraph), on the real Lua UI and Config
 * libraries; Game and Input stand in (they need a GameSession). LightLab's
 * own config vars are declared here, as its module declares them. */
NIMBLE_CONFIG_FLOAT(GTestLookSensitivity, "LightLab", "LookSensitivity", 1.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GTestFlySpeed, "LightLab", "FlySpeed", 600.0f, nullptr);

static void TestLightLabGraph()
{
    std::printf("[lightlab graph]\n");
    EngineCore::ConfigVar* Shadows = EngineCore::ConfigFindVarByPath("Shadows.Enabled");
    Expect(Shadows != nullptr, "the shadows config var is there to set");
    if(!Shadows) return;
    const bool bShadowsBefore = Shadows->GetBool();
    Shadows->SetBool(true);

    EngineCore::FilesystemProvider Game(std::string(UI_REPO_ROOT) + "/Games/LightLab");
    EngineCore::IFileProvider* Previous = EngineCore::GetFileProvider();
    EngineCore::SetFileProvider(&Game);

    GameCore::World World;
    GameCore::ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "a script engine for LightLab's menu");
    Scripts.BindWorld(&World);
    Expect(Scripts.RunString("Game = { SetPaused = function(p) Paused = p end }\n"
                             "Input = { SetMouseCaptured = function(c) Captured = c end }\n", "stand-ins"), "Game and Input stand in");
    UISystem System;
    System.SetScriptEngine(&Scripts);
    SetScriptUISystem(&System);

    const std::string UserFile = (std::filesystem::temp_directory_path() / "NimbleUILightLabUser.ini").string();
    std::filesystem::remove(UserFile);
    EngineCore::ConfigSetUserFile(UserFile);
    System.SetValue("LightLab.Page", UIValue::Number(1));   // left on Settings last time
    const UIDocumentHandle Menu = System.Open("Content/UI/PauseMenu.nmui");
    Expect(Menu != UI_DOCUMENT_INVALID, "the pause menu opens with its logic");
    System.RunQueuedActions();
    System.Tick(0.016f);
    System.Update(UIVec2{ 1280, 720 });
    UIDocumentInstance* Doc = System.Find(Menu);
    if(!Doc) { EngineCore::SetFileProvider(Previous); return; }
    const UIValue* Published = System.GetValue("LightLab.Shadows");
    Expect(Published && Published->B && System.GetValue("LightLab.LookSensitivity")->AsNumber() == 1.0,
           "On Document Open fills the store from the config vars");
    auto* Pages = dynamic_cast<UISwitcher*>(Doc->FindWidget("Pages"));
    Expect(Pages && Pages->ActiveIndex == 0, "and shows the first page");

    const auto Click = [&](const char* InWidget)
    {
        System.Update(UIVec2{ 1280, 720 });
        const UIVec2 At = CentreOf(*Doc, InWidget);
        System.ProcessInput(Pointer(At, true, false));
        System.ProcessInput(Pointer(At, false, true));
        System.RunQueuedActions();
        System.Tick(0.016f);
    };
    Click("SettingsButton");
    Expect(Pages->ActiveIndex == 1, "Settings shows its page");
    Click("Shadows");
    Expect(!Shadows->GetBool(), "unticking Shadows turns the config var off, through the graph");

    Doc->SetFocus(int32_t(Doc->FindWidget("LookSensitivity")->GetIndex()));
    UIInputFrame Right;
    Right.bNavRight = true;
    System.ProcessInput(Right);
    System.RunQueuedActions();
    Expect(std::fabs(GTestLookSensitivity.GetFloat() - 1.05f) < 1e-4f, "Right on the slider steps the look sensitivity var");

    UIInputFrame Back;
    Back.bBack = true;
    System.ProcessInput(Back);
    System.RunQueuedActions();
    System.Tick(0.016f);
    Expect(Pages->ActiveIndex == 0 && System.IsOpen("Content/UI/PauseMenu.nmui"), "Back from Settings returns to the first page");
    System.Update(UIVec2{ 1280, 720 });
    System.ProcessInput(Back);
    System.RunQueuedActions();
    Expect(!System.IsOpen("Content/UI/PauseMenu.nmui") && Scripts.RunString("assert(Paused == false and Captured == true)", "resumed"),
           "Back from there resumes: closed, unpaused, the mouse taken back");
    Expect(Scripts.GetErrorCount() == 0, "the menu's logic raised no Lua errors");
    std::ifstream Saved(UserFile);
    const std::string SavedText((std::istreambuf_iterator<char>(Saved)), std::istreambuf_iterator<char>());
    Expect(SavedText.find("[Shadows]") != std::string::npos && SavedText.find("Enabled=false") != std::string::npos
           && SavedText.find("LookSensitivity=1.05") != std::string::npos,
           "closing the menu saved the player's changes to their user file");
    EngineCore::ConfigSetUserFile("");
    std::filesystem::remove(UserFile);

    Shadows->SetBool(bShadowsBefore);
    GTestLookSensitivity.ResetToDefault();
    System.SetScriptEngine(nullptr);
    SetScriptUISystem(nullptr);
    Scripts.UnbindWorld();
    Scripts.Shutdown();
    EngineCore::SetFileProvider(Previous);
}

/* A document opened mid-frame (by game code, after input) shows its graph's
 * OnOpen values on the frame it first paints. */
static void TestOpenBeforePaint()
{
    std::printf("[open before paint]\n");
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleUIOpenPaint";
    std::filesystem::create_directories(Dir);
    EngineCore::FilesystemProvider Disk(Dir.string());
    EngineCore::IFileProvider* Previous = EngineCore::GetFileProvider();
    EngineCore::SetFileProvider(&Disk);
    std::ofstream(Dir / "Hud.ui.nmgraph", std::ios::binary | std::ios::trunc)
        << "<NodeGraph Version=\"1\" Schema=\"UI\"><gen_code><Output Extension=\".lua\"><![CDATA["
           "return { OnOpen = function(self) UI.SetNumber('Hp', 7) end }]]></Output></gen_code></NodeGraph>\n";
    std::ofstream(Dir / "Hud.nmui", std::ios::binary | std::ios::trunc)
        << R"XML(<UI ScaleMode="None" Graph="Hud.ui.nmgraph"><Canvas><Text Name="Hp" AutoSize="true" Text="HP {Bind:Hp}"/></Canvas></UI>)XML";

    GameCore::World World;
    GameCore::ScriptEngine Scripts;
    Scripts.Initialize();
    Scripts.BindWorld(&World);
    UISystem System;
    System.SetScriptEngine(&Scripts);
    SetScriptUISystem(&System);
    const UIDocumentHandle Hud = System.Open("Hud.nmui");
    System.Update(UIVec2{ 800, 600 });   // no RunQueuedActions or Tick between: as when opened from gameplay
    const UIText* Hp = Hud ? dynamic_cast<UIText*>(System.Find(Hud)->FindWidget("Hp")) : nullptr;
    Expect(Hp && Hp->Text == "HP 7", "the first painted frame already shows what OnOpen set");

    System.SetScriptEngine(nullptr);
    SetScriptUISystem(nullptr);
    Scripts.UnbindWorld();
    Scripts.Shutdown();
    EngineCore::SetFileProvider(Previous);
    std::filesystem::remove_all(Dir);
}

// === Hot reload ===============================================================

static void TestHotReload()
{
    std::printf("[hot reload]\n");
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleUIReload";
    std::filesystem::create_directories(Dir);
    EngineCore::FilesystemProvider Disk(Dir.string());
    EngineCore::IFileProvider* Previous = EngineCore::GetFileProvider();
    EngineCore::SetFileProvider(&Disk);

    const auto Write = [&Dir](const char* InXml)
    {
        std::ofstream File(Dir / "Reload.nmui", std::ios::binary | std::ios::trunc);
        File << InXml;
    };
    Write(R"XML(<UI><Canvas><Button Name="A" Focusable="true" InitialFocus="true"/></Canvas></UI>)XML");
    UISystem System;
    const UIDocumentHandle Handle = System.Open("Reload.nmui");
    Expect(Handle != UI_DOCUMENT_INVALID, "Open loads through the file provider");
    Expect(System.Open("Reload.nmui") == Handle, "opening it again returns it");

    Write(R"XML(<UI><Canvas><Button Name="A" Opacity="0.25"/><Button Name="B"/></Canvas></UI>)XML");
    System.Tick(0.016f);
    Expect(System.Find(Handle)->GetWidgetCount() == 2, "until invalidated, the cached document stays");
    UIAssetCache::Invalidate("Reload.nmui");
    System.Tick(0.016f);
    UIDocumentInstance* Reloaded = System.Find(Handle);
    Expect(Reloaded && Reloaded->GetWidgetCount() == 3 && Reloaded->GetFocused() == int32_t(Reloaded->FindWidget("A")->GetIndex()),
           "Invalidate rebuilds it, focus kept by name, the handle unchanged");

    EngineCore::SetFileProvider(Previous);
    std::filesystem::remove_all(Dir);
}

// === Cost =====================================================================

static void TestCost()
{
    std::printf("[cost]\n");

    /* 500 widgets: 100 rows of a box holding a border, a text and two spacers. */
    std::string Xml = "<UI ReferenceSize=\"1920,1080\"><Canvas>\n";
    for(int Row = 0; Row < 100; ++Row)
    {
        Xml += "<HorizontalBox Position=\"0," + std::to_string(Row * 10) + "\" Size=\"1000,10\">"
               "<Border Size=\"10,10\" Background.Tint=\"#336699FF\"/><Text Text=\"Row " + std::to_string(Row) + "\" FontSize=\"8\"/>"
               "<Spacer Fill=\"1\"/><Spacer Size=\"5,5\"/></HorizontalBox>\n";
    }
    Xml += "</Canvas></UI>";
    auto Asset = Parse(Xml, "the 500-widget document parses");

    UISystem System;
    const UIDocumentHandle Handle = System.OpenAsset(Asset, "Test/Cost.nmui");
    UIDocumentInstance& Doc = *System.Find(Handle);
    Expect(Doc.GetWidgetCount() == 501, "501 widgets");

    using Clock = std::chrono::steady_clock;
    const auto Micro = [](const Clock::duration InTime) { return double(std::chrono::duration_cast<std::chrono::nanoseconds>(InTime).count()) / 1000.0; };

    const auto T0 = Clock::now();
    System.Update(UIVec2{ 1920, 1080 });
    const auto T1 = Clock::now();
    Doc.MarkLayoutDirty();
    System.Update(UIVec2{ 1920, 1080 });
    const auto T2 = Clock::now();

    /* Warm, unchanged frames: input, tick and update allocate nothing. */
    UIInputFrame Idle = Pointer(UIVec2{ 1500, 900 }, false, false);
    for(int Warm = 0; Warm < 3; ++Warm)
    {
        System.ProcessInput(Idle);
        System.RunQueuedActions();
        System.Tick(0.016f);
        System.Update(UIVec2{ 1920, 1080 });
    }
    const uint64_t Before = GAllocations.load();
    const auto T3 = Clock::now();
    for(int Frame = 0; Frame < 100; ++Frame)
    {
        System.ProcessInput(Idle);
        System.RunQueuedActions();
        System.Tick(0.016f);
        System.Update(UIVec2{ 1920, 1080 });
    }
    const auto T4 = Clock::now();
    const uint64_t Allocated = GAllocations.load() - Before;
    Expect(Allocated == 0, "an unchanged frame allocates nothing");

    Doc.MarkPaintDirty();
    const auto T5 = Clock::now();
    System.Update(UIVec2{ 1920, 1080 });
    const auto T6 = Clock::now();
    int32_t Hit = 0;
    const auto T7 = Clock::now();
    for(int Each = 0; Each < 100; ++Each) { Hit += Doc.HitTest(UIVec2{ float(Each * 7), float(Each * 9) }); }
    const auto T8 = Clock::now();

    std::printf("    500 widgets: first update %.0f us, relayout %.0f us, repaint %.0f us, "
                "unchanged frame %.2f us, hit test %.2f us (%llu allocations over 100 unchanged frames)\n",
                Micro(T1 - T0), Micro(T2 - T1), Micro(T6 - T5), Micro(T4 - T3) / 100.0, Micro(T8 - T7) / 100.0,
                static_cast<unsigned long long>(Allocated));
    (void)Hit;
}

#if WITH_EDITOR
/*
 * OPT-IN, on a real GPU: UI_TESTS_GPU=Vulkan (or D3D12), run from a runtime
 * directory the build staged (Binaries/<Platform>_<Config>), where the cooked
 * shaders, the engine's font and Sandbox's Content/UI are. Draws the shipped
 * documents through the real renderer and reads them back; UI_TESTS_DUMP=<dir>
 * writes each as a PPM to look at.
 */
static void TestOnGpu()
{
    const char* Gpu = std::getenv("UI_TESTS_GPU");
    if(!Gpu || !Gpu[0])
    {
        std::printf("[on the GPU] skipped: set UI_TESTS_GPU=Vulkan or D3D12\n");
        return;
    }
    std::printf("[on the GPU: %s]\n", Gpu);
    const RenderCore::ERenderBackend Backend = std::string(Gpu) == "D3D12" ? RenderCore::ERenderBackend::D3D12 : RenderCore::ERenderBackend::Vulkan;

    EngineCore::FilesystemProvider Disk;   // the working directory: the staged runtime
    EngineCore::IFileProvider* Previous = EngineCore::GetFileProvider();
    EngineCore::SetFileProvider(&Disk);

    RenderCore::Renderer Rend;
    const bool bInit = Rend.Initialize(Backend, EngineCore::WindowHandle());
    Expect(bInit, "the renderer starts on the GPU with no window");
    if(bInit)
    {
        const EngineCore::Size Viewport(1280, 720);
        const RenderCore::RenderView View = RenderCore::RenderView::MakeOrthographic(Viewport, EngineCore::Vector3(0.0, 0.0, -10.0), -1.0f, 1000.0f);
        Rend.SetViewOverride(&View);
        Rend.SetClearGradient(RenderCore::RENDER_DEFAULT_SCENE, EngineCore::Color(52, 74, 104, 255), EngineCore::Color(18, 22, 30, 255));
        GameCore::World World;
        Rend.BindWorld(World);

        uint64_t Frame = 1;
        const char* DumpDir = std::getenv("UI_TESTS_DUMP");
        const auto Shot = [&](UISystem& InSystem, const char* InName, EngineCore::Image& OutPixels)
        {
            InSystem.Tick(0.016f);
            InSystem.Update(UIVec2{ float(Viewport.Width), float(Viewport.Height) });
            Rend.EndFrame(RenderCore::FrameInfo{ Frame++, 0.016, Viewport });
            Rend.WaitForPreviousFrame();
            const uint64_t Request = Rend.RequestSceneCapture();
            for(int Each = 0; Each < 2; ++Each)
            {
                Rend.EndFrame(RenderCore::FrameInfo{ Frame++, 0.016, Viewport });
                Rend.WaitForPreviousFrame();
            }
            std::string Error;
            const bool bTaken = Request != 0 && Rend.TakeMaterialPreview(Request, OutPixels, Error) && OutPixels.GetWidth() == int(Viewport.Width);
            if(bTaken && DumpDir && DumpDir[0])
            {
                std::ofstream File(std::string(DumpDir) + "/" + InName + ".ppm", std::ios::binary);
                File << "P6\n" << OutPixels.GetWidth() << " " << OutPixels.GetHeight() << "\n255\n";
                const bool bBgra = OutPixels.GetFormat() == EngineCore::EImageFormat::BGRA8;
                for(int Y = 0; Y < OutPixels.GetHeight(); ++Y)
                {
                    for(int X = 0; X < OutPixels.GetWidth(); ++X)
                    {
                        const uint8_t* Px = OutPixels.GetData() + OutPixels.GetStride() * size_t(Y) + size_t(X) * 4;
                        const char Rgb[3] = { char(bBgra ? Px[2] : Px[0]), char(Px[1]), char(bBgra ? Px[0] : Px[2]) };
                        File.write(Rgb, 3);
                    }
                }
            }
            return bTaken;
        };
        const auto Brightness = [](const EngineCore::Image& InPixels, const int InX, const int InY)
        {
            const uint8_t* Px = InPixels.GetData() + InPixels.GetStride() * size_t(InY) + size_t(InX) * 4;
            return int(Px[0]) + int(Px[1]) + int(Px[2]);
        };

        /* Sandbox: its HUD with values, then its menu over it. */
        {
            UISystem System;
            System.SetRenderer(&Rend);
            System.SetValue("Player0.Score", UIValue::Number(1250));
            System.SetValue("Player0.Lives", UIValue::Number(3));
            System.SetValue("Game.Phase", UIValue::String("Serving"));
            Expect(System.Open("Content/UI/HUD.nmui") != UI_DOCUMENT_INVALID, "Sandbox's HUD loads from the runtime directory");
            EngineCore::Image Hud;
            Expect(Shot(System, "sandbox_hud", Hud), "the HUD frame reads back");
            UIDocumentInstance* HudDoc = System.Find("Content/UI/HUD.nmui");
            if(Hud.GetWidth() > 0 && HudDoc)
            {
                const UIRect Score = HudDoc->GetPixelRect(HudDoc->FindWidget("Score")->GetIndex());
                int Brightest = 0;
                for(int Y = int(Score.Y); Y < int(Score.Y + Score.H); ++Y)
                {
                    for(int X = int(Score.X); X < int(Score.X + Score.W); ++X) { Brightest = std::max(Brightest, Brightness(Hud, X, Y)); }
                }
                Expect(Score.W > 40.0f && Brightest > 600, "the bound score is drawn as text, in white");
            }

            Expect(System.Open("Content/UI/MainMenu.nmui") != UI_DOCUMENT_INVALID, "Sandbox's main menu loads");
            EngineCore::Image Menu;
            Expect(Shot(System, "sandbox_menu", Menu), "the menu frame reads back");
            UIDocumentInstance* MenuDoc = System.Find("Content/UI/MainMenu.nmui");
            if(Menu.GetWidth() > 0 && MenuDoc)
            {
                const UIRect Play = MenuDoc->GetPixelRect(MenuDoc->FindWidget("Play")->GetIndex());
                Expect(Play.W > 100.0f && Play.X > 300.0f && Play.X + Play.W < 980.0f, "the menu's buttons are centred");
                /* Play has focus (InitialFocus): its hovered blue, not the normal grey. */
                const uint8_t* Px = Menu.GetData() + Menu.GetStride() * size_t(Play.Y + 4) + size_t(Play.X + 6) * 4;
                const bool bBgra = Menu.GetFormat() == EngineCore::EImageFormat::BGRA8;
                const int R = bBgra ? Px[2] : Px[0], B = bBgra ? Px[0] : Px[2];
                Expect(B > R + 60, "the focused button shows its hovered look");
            }

            /* Its settings, on the Graphics page: tabs, marks and pages switched by one value. */
            System.SetValue("Settings.Page", UIValue::Number(1));
            Expect(System.Open("Content/UI/Settings.nmui") != UI_DOCUMENT_INVALID, "Sandbox's settings menu loads");
            EngineCore::Image Settings;
            Expect(Shot(System, "sandbox_settings", Settings), "the settings frame reads back");
            UIDocumentInstance* SettingsDoc = System.Find("Content/UI/Settings.nmui");
            Expect(SettingsDoc && SettingsDoc->IsShown(SettingsDoc->FindWidget("Graphics")->GetIndex())
                   && !SettingsDoc->IsShown(SettingsDoc->FindWidget("Input")->GetIndex()), "it shows the bound page");
            System.ClearRenderer();
        }

        /* LightLab's pause menu, from the source tree. */
        {
            std::ifstream File(std::string(UI_REPO_ROOT) + "/Games/LightLab/Content/UI/PauseMenu.nmui", std::ios::binary);
            const std::string Xml((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
            UISystem System;
            System.SetRenderer(&Rend);
            Expect(System.OpenAsset(Parse(Xml, "LightLab's pause menu parses"), "Content/UI/PauseMenu.nmui") != UI_DOCUMENT_INVALID,
                   "LightLab's pause menu opens");
            EngineCore::Image Pause;
            Expect(Shot(System, "lightlab_pause", Pause), "the pause frame reads back");

            /* Its Settings page, with the values LightLab publishes as it opens. */
            System.SetValue("LightLab.Shadows", UIValue::Bool(true));
            System.SetValue("LightLab.BakedLighting", UIValue::Bool(false));
            System.SetValue("LightLab.LookSensitivity", UIValue::Number(1.25));
            System.SetValue("LightLab.FlySpeed", UIValue::Number(600));
            UIDocumentInstance* PauseDoc = System.Find("Content/UI/PauseMenu.nmui");
            auto* Pages = PauseDoc ? dynamic_cast<UISwitcher*>(PauseDoc->FindWidget("Pages")) : nullptr;
            Expect(Pages && Pages->ShowPage("Settings"), "it has a Settings page");
            EngineCore::Image SettingsShot;
            Expect(Shot(System, "lightlab_settings", SettingsShot), "the settings frame reads back");
            if(PauseDoc && Pages)
            {
                auto* Shadows = dynamic_cast<UICheckBox*>(PauseDoc->FindWidget("Shadows"));
                auto* Look    = dynamic_cast<UISlider*>(PauseDoc->FindWidget("LookSensitivity"));
                Expect(Shadows && Shadows->bChecked && PauseDoc->GetFocused() == int32_t(Shadows->GetIndex()),
                       "the page shows the store's values, focus on its first setting");
                Expect(Look && Near(Look->Value, 1.25f) && PauseDoc->GetPixelRect(Look->GetIndex()).W > 200.0f, "the sliders are laid out with their values");
            }
            System.ClearRenderer();
        }

        Rend.UnbindWorld();
    }
    Rend.Shutdown();
    EngineCore::SetFileProvider(Previous);
}
#endif

int main()
{
    EngineCore::SetLogHandler(&TestLogHandler);

    /* The engine's font, as a game finds it: Engine/Content/Fonts under the root. */
    EngineCore::FilesystemProvider Repo(UI_REPO_ROOT);
    EngineCore::SetFileProvider(&Repo);

    TestRegistry();
    TestValuesAndActions();
    TestLayout();
    TestPaint();
    TestText();
    TestStylesAndFiles();
    TestSwitcher();
    TestInputWidgets();
    TestScrollBox();
    TestStructureEditing();
    TestInputAndActions();
    TestScriptedActions();
    TestUIGraphs();
    TestOpenBeforePaint();
    TestLightLabGraph();
    TestHotReload();
    TestCost();
#if WITH_EDITOR
    TestOnGpu();
#endif

    std::printf("\n%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
