#if WITH_EDITOR

#include "Private/Editor/EditorTheme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"

namespace
{
    enum class ETheme : int32_t
    {
        System = 0,
        Light  = 1,
        Dark   = 2,
    };

    bool bInstalled = false;
    bool bApplying  = false;

    void Apply();

    void OnThemeChanged(const EngineCore::ConfigVar&)
    {
        /* Also fires while main() loads the config, before Install: Install
         * applies it then. */
        if(bInstalled)
        {
            Apply();
        }
    }

    QPalette MakeDarkPalette()
    {
        const QColor Text(220, 220, 222);
        const QColor Disabled(120, 120, 124);

        QPalette P;
        P.setColor(QPalette::Window,          QColor(45, 45, 48));
        P.setColor(QPalette::WindowText,      Text);
        P.setColor(QPalette::Base,            QColor(30, 30, 32));
        P.setColor(QPalette::AlternateBase,   QColor(40, 40, 43));
        P.setColor(QPalette::ToolTipBase,     QColor(55, 55, 60));
        P.setColor(QPalette::ToolTipText,     Text);
        P.setColor(QPalette::PlaceholderText, QColor(140, 140, 145));
        P.setColor(QPalette::Text,            Text);
        P.setColor(QPalette::Button,          QColor(55, 55, 59));
        P.setColor(QPalette::ButtonText,      Text);
        P.setColor(QPalette::BrightText,      QColor(255, 90, 90));
        P.setColor(QPalette::Light,           QColor(80, 80, 85));
        P.setColor(QPalette::Midlight,        QColor(66, 66, 70));
        P.setColor(QPalette::Mid,             QColor(90, 90, 95));
        P.setColor(QPalette::Dark,            QColor(28, 28, 30));
        P.setColor(QPalette::Shadow,          QColor(15, 15, 16));
        P.setColor(QPalette::Highlight,       QColor(42, 125, 210));
        P.setColor(QPalette::HighlightedText, Qt::white);
        P.setColor(QPalette::Link,            QColor(90, 160, 240));
        P.setColor(QPalette::LinkVisited,     QColor(160, 130, 230));

        for(const QPalette::ColorRole Role : { QPalette::WindowText, QPalette::Text, QPalette::ButtonText })
        {
            P.setColor(QPalette::Disabled, Role, Disabled);
        }
        P.setColor(QPalette::Disabled, QPalette::Highlight,       QColor(70, 70, 75));
        P.setColor(QPalette::Disabled, QPalette::HighlightedText, Disabled);
        return P;
    }
}

/* Registered here, next to what reads it; Edit > Editor Settings shows it. */
NIMBLE_CONFIG_INT(GEditorTheme, "Editor", "Theme", int32_t(ETheme::System), &OnThemeChanged);

namespace
{
    void Apply()
    {
        if(!qApp || bApplying)
        {
            return;
        }
        bApplying = true;   // setColorScheme below emits colorSchemeChanged

        if(QApplication::style()->name().compare(QStringLiteral("fusion"), Qt::CaseInsensitive) != 0)
        {
            QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        }

        QStyleHints* Hints = QGuiApplication::styleHints();
        ETheme Theme = ETheme(GEditorTheme.GetInt());
        if(Theme != ETheme::Light && Theme != ETheme::Dark)
        {
            Theme = ETheme::System;
        }

        if(Theme == ETheme::System)
        {
            /* Drop any earlier request, so colorScheme() is the system's again. */
            Hints->unsetColorScheme();
        }
        else
        {
            Hints->setColorScheme(Theme == ETheme::Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        }
        const bool bDark = Theme == ETheme::System ? Hints->colorScheme() == Qt::ColorScheme::Dark
                                                   : Theme == ETheme::Dark;

        QApplication::setPalette(bDark ? MakeDarkPalette() : QApplication::style()->standardPalette());

        bApplying = false;
    }
}

namespace EditorTheme
{
    void Install()
    {
        if(bInstalled)
        {
            return;
        }
        bInstalled = true;

        /* The system switching light/dark while the editor runs. Only
         * matters in System mode; Apply is cheap and idempotent either way. */
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, []()
        {
            if(ETheme(GEditorTheme.GetInt()) != ETheme::Light && ETheme(GEditorTheme.GetInt()) != ETheme::Dark)
            {
                Apply();
            }
        });

        Apply();
    }
}

#endif // WITH_EDITOR
