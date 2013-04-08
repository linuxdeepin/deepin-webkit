/*
 * Copyright (C) 2006, 2007 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Igalia S.L
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "ContextMenuController.h"

#if ENABLE(CONTEXT_MENUS)

#include "BackForwardController.h"
#include "Chrome.h"
#include "ContextMenu.h"
#include "ContextMenuClient.h"
#include "ContextMenuItem.h"
#include "ItemSelectedEvent.h"
#include "ContextMenuProvider.h"
#include "Document.h"
#include "DocumentFragment.h"
#include "DocumentLoader.h"
#include "Editor.h"
#include "EditorClient.h"
#include "Event.h"
#include "EventHandler.h"
#include "EventNames.h"
#include "FormState.h"
#include "Frame.h"
#include "FrameLoadRequest.h"
#include "FrameLoader.h"
#include "FrameLoaderClient.h"
#include "FrameSelection.h"
#include "HTMLFormElement.h"
#include "HitTestRequest.h"
#include "HitTestResult.h"
#include "InspectorController.h"
#include "LocalizedStrings.h"
#include "MouseEvent.h"
#include "NavigationAction.h"
#include "Node.h"
#include "Page.h"
#include "RenderLayer.h"
#include "RenderObject.h"
#include "ReplaceSelectionCommand.h"
#include "ResourceRequest.h"
#include "Settings.h"
#include "TextIterator.h"
#include "UserTypingGestureIndicator.h"
#include "WindowFeatures.h"
#include "markup.h"
#include <wtf/unicode/Unicode.h>

using namespace WTF;
using namespace Unicode;

namespace WebCore {

static void openNewWindow(const KURL& urlToLoad, Frame* frame)
{
    if (Page* oldPage = frame->page()) {
        FrameLoadRequest request(frame->document()->securityOrigin(), ResourceRequest(urlToLoad, frame->loader()->outgoingReferrer()));
        if (Page* newPage = oldPage->chrome()->createWindow(frame, request, WindowFeatures(), NavigationAction(request.resourceRequest()))) {
            newPage->mainFrame()->loader()->loadFrameRequest(request, false, false, 0, 0, MaybeSendReferrer);
            newPage->chrome()->show();
        }
    }
}

ContextMenuController::ContextMenuController(Page* page, ContextMenuClient* client)
    : m_page(page)
    , m_client(client)
{
    ASSERT_ARG(page, page);
    ASSERT_ARG(client, client);
}

ContextMenuController::~ContextMenuController()
{
    m_client->contextMenuDestroyed();
}

PassOwnPtr<ContextMenuController> ContextMenuController::create(Page* page, ContextMenuClient* client)
{
    return adoptPtr(new ContextMenuController(page, client));
}

void ContextMenuController::clearContextMenu()
{
    m_contextMenu.clear();
    if (m_menuProvider)
        m_menuProvider->contextMenuCleared();
    m_menuProvider = 0;
}

void ContextMenuController::handleContextMenuEvent(Event* event)
{
    m_element = toHTMLElement(event->target()->toNode());

    HTMLElement* self = m_element;
    DeepinMenu* deepinmenu = NULL;
    while (self != NULL) {
        deepinmenu = self->contextMenu();
        if (deepinmenu)
            break;
        self = toHTMLElement(self->parentElement());
    }
    m_element = self;

    if (deepinmenu != NULL) {
        m_contextMenu = deepinmenu->contextMenu();
    }
    if (!m_contextMenu) {
        //build default menu
        m_contextMenu = createContextMenu(event);
        if (m_contextMenu)
            populate();
    }
    if (!m_contextMenu)
        return;
    showContextMenu(event);
}

static PassOwnPtr<ContextMenuItem> separatorItem()
{
    return adoptPtr(new ContextMenuItem(SeparatorType, ContextMenuItemTagNoAction, String()));
}

void ContextMenuController::showContextMenu(Event* event, PassRefPtr<ContextMenuProvider> menuProvider)
{
    m_menuProvider = menuProvider;

    m_contextMenu = createContextMenu(event);
    if (!m_contextMenu) {
        clearContextMenu();
        return;
    }

    m_menuProvider->populateContextMenu(m_contextMenu.get());
    if (m_hitTestResult.isSelected()) {
        appendItem(*separatorItem(), m_contextMenu.get());
    }
    showContextMenu(event);
    event->setDefaultHandled();
}

PassOwnPtr<ContextMenu> ContextMenuController::createContextMenu(Event* event)
{
    if (!event->isMouseEvent())
        return nullptr;

    MouseEvent* mouseEvent = static_cast<MouseEvent*>(event);
    HitTestResult result(mouseEvent->absoluteLocation());

    if (Frame* frame = event->target()->toNode()->document()->frame())
        result = frame->eventHandler()->hitTestResultAtPoint(mouseEvent->absoluteLocation(), false);

    if (!result.innerNonSharedNode())
        return nullptr;

    m_hitTestResult = result;

    return adoptPtr(new ContextMenu);
}

void ContextMenuController::showContextMenu(Event* event)
{
    PlatformMenuDescription customMenu = m_client->getCustomMenuFromDefaultItems(m_contextMenu.get());
    m_contextMenu->setPlatformDescription(customMenu);

    event->setDefaultHandled();
}

void ContextMenuController::contextMenuItemSelected(ContextMenuItem* item)
{
    ASSERT(item->type() == ActionType || item->type() == CheckableActionType);

    int id = item->action() - ContextMenuItemBaseApplicationTag;
    if (id > 0) {
        PassRefPtr<ItemSelectedEvent> event = ItemSelectedEvent::create();
        event->initEvent(eventNames().itemselectedEvent, false, false);
        event->setID(id);
        event->setTitle(item->title());
        event->setTarget(m_element);
        event->setCurrentTarget(m_element);
        event->setEventPhase(Event::AT_TARGET);
        m_element->fireEventListeners(event.get());

        // FIXME: need this?
        m_client->contextMenuItemSelected(item, contextMenu());
        return;
    }

    if (item->action() >= ContextMenuItemBaseCustomTag) {
        ASSERT(m_menuProvider);
        m_menuProvider->contextMenuItemSelected(item);
        return;
    }
    Frame* frame = m_hitTestResult.innerNonSharedNode()->document()->frame();
    if (!frame)
        return;

    switch (item->action()) {
    case ContextMenuItemTagOpenLinkInNewWindow:
        openNewWindow(m_hitTestResult.absoluteLinkURL(), frame);
        break;
    case ContextMenuItemTagDownloadLinkToDisk:
        // FIXME: Some day we should be able to do this from within WebCore.
        m_client->downloadURL(m_hitTestResult.absoluteLinkURL());
        break;
    case ContextMenuItemTagCopyLinkToClipboard:
        frame->editor()->copyURL(m_hitTestResult.absoluteLinkURL(), m_hitTestResult.textContent());
        break;
    case ContextMenuItemTagOpenImageInNewWindow:
        openNewWindow(m_hitTestResult.absoluteImageURL(), frame);
        break;
    case ContextMenuItemTagDownloadImageToDisk:
        // FIXME: Some day we should be able to do this from within WebCore.
        m_client->downloadURL(m_hitTestResult.absoluteImageURL());
        break;
    case ContextMenuItemTagCopyImageToClipboard:
        // FIXME: The Pasteboard class is not written yet
        // For now, call into the client. This is temporary!
        frame->editor()->copyImage(m_hitTestResult);
        break;
    case ContextMenuItemTagCopyImageUrlToClipboard:
        frame->editor()->copyURL(m_hitTestResult.absoluteImageURL(), m_hitTestResult.textContent());
        break;
    case ContextMenuItemTagOpenMediaInNewWindow:
        openNewWindow(m_hitTestResult.absoluteMediaURL(), frame);
        break;
    case ContextMenuItemTagCopyMediaLinkToClipboard:
        frame->editor()->copyURL(m_hitTestResult.absoluteMediaURL(), m_hitTestResult.textContent());
        break;
    case ContextMenuItemTagToggleMediaControls:
        m_hitTestResult.toggleMediaControlsDisplay();
        break;
    case ContextMenuItemTagToggleMediaLoop:
        m_hitTestResult.toggleMediaLoopPlayback();
        break;
    case ContextMenuItemTagEnterVideoFullscreen:
        m_hitTestResult.enterFullscreenForVideo();
        break;
    case ContextMenuItemTagMediaPlayPause:
        m_hitTestResult.toggleMediaPlayState();
        break;
    case ContextMenuItemTagMediaMute:
        m_hitTestResult.toggleMediaMuteState();
        break;
    case ContextMenuItemTagOpenFrameInNewWindow: {
        DocumentLoader* loader = frame->loader()->documentLoader();
        if (!loader->unreachableURL().isEmpty())
            openNewWindow(loader->unreachableURL(), frame);
        else
            openNewWindow(loader->url(), frame);
        break;
    }
    case ContextMenuItemTagCopy:
        frame->editor()->copy();
        break;
    case ContextMenuItemTagGoBack:
        if (Page* page = frame->page())
            page->backForward()->goBackOrForward(-1);
        break;
    case ContextMenuItemTagGoForward:
        if (Page* page = frame->page())
            page->backForward()->goBackOrForward(1);
        break;
    case ContextMenuItemTagStop:
        frame->loader()->stop();
        break;
    case ContextMenuItemTagReload:
        frame->loader()->reload();
        break;
    case ContextMenuItemTagCut:
        frame->editor()->command("Cut").execute();
        break;
    case ContextMenuItemTagPaste:
        frame->editor()->command("Paste").execute();
        break;
    case ContextMenuItemTagDelete:
        frame->editor()->performDelete();
        break;
    case ContextMenuItemTagSelectAll:
        frame->editor()->command("SelectAll").execute();
        break;
    case ContextMenuItemTagSpellingGuess:
        ASSERT(frame->editor()->selectedText().length());
        if (frame->editor()->shouldInsertText(item->title(), frame->selection()->toNormalizedRange().get(), EditorInsertActionPasted)) {
            Document* document = frame->document();
            RefPtr<ReplaceSelectionCommand> command = ReplaceSelectionCommand::create(document, createFragmentFromMarkup(document, item->title(), ""), ReplaceSelectionCommand::SelectReplacement | ReplaceSelectionCommand::MatchStyle | ReplaceSelectionCommand::PreventNesting);
            applyCommand(command);
            frame->selection()->revealSelection(ScrollAlignment::alignToEdgeIfNeeded);
        }
        break;
    case ContextMenuItemTagIgnoreSpelling:
        frame->editor()->ignoreSpelling();
        break;
    case ContextMenuItemTagLearnSpelling:
        frame->editor()->learnSpelling();
        break;
    case ContextMenuItemTagSearchWeb:
        m_client->searchWithGoogle(frame);
        break;
    case ContextMenuItemTagLookUpInDictionary:
        // FIXME: Some day we may be able to do this from within WebCore.
        m_client->lookUpInDictionary(frame);
        break;
    case ContextMenuItemTagOpenLink:
        if (Frame* targetFrame = m_hitTestResult.targetFrame())
            targetFrame->loader()->loadFrameRequest(FrameLoadRequest(frame->document()->securityOrigin(), ResourceRequest(m_hitTestResult.absoluteLinkURL(), frame->loader()->outgoingReferrer())), false, false, 0, 0, MaybeSendReferrer);
        else
            openNewWindow(m_hitTestResult.absoluteLinkURL(), frame);
        break;
    case ContextMenuItemTagBold:
        frame->editor()->command("ToggleBold").execute();
        break;
    case ContextMenuItemTagItalic:
        frame->editor()->command("ToggleItalic").execute();
        break;
    case ContextMenuItemTagUnderline:
        frame->editor()->toggleUnderline();
        break;
    case ContextMenuItemTagOutline:
        // We actually never enable this because CSS does not have a way to specify an outline font,
        // which may make this difficult to implement. Maybe a special case of text-shadow?
        break;
    case ContextMenuItemTagStartSpeaking: {
        ExceptionCode ec;
        RefPtr<Range> selectedRange = frame->selection()->toNormalizedRange();
        if (!selectedRange || selectedRange->collapsed(ec)) {
            Document* document = m_hitTestResult.innerNonSharedNode()->document();
            selectedRange = document->createRange();
            selectedRange->selectNode(document->documentElement(), ec);
        }
        m_client->speak(plainText(selectedRange.get()));
        break;
    }
    case ContextMenuItemTagStopSpeaking:
        m_client->stopSpeaking();
        break;
    case ContextMenuItemTagDefaultDirection:
        frame->editor()->setBaseWritingDirection(NaturalWritingDirection);
        break;
    case ContextMenuItemTagLeftToRight:
        frame->editor()->setBaseWritingDirection(LeftToRightWritingDirection);
        break;
    case ContextMenuItemTagRightToLeft:
        frame->editor()->setBaseWritingDirection(RightToLeftWritingDirection);
        break;
    case ContextMenuItemTagTextDirectionDefault:
        frame->editor()->command("MakeTextWritingDirectionNatural").execute();
        break;
    case ContextMenuItemTagTextDirectionLeftToRight:
        frame->editor()->command("MakeTextWritingDirectionLeftToRight").execute();
        break;
    case ContextMenuItemTagTextDirectionRightToLeft:
        frame->editor()->command("MakeTextWritingDirectionRightToLeft").execute();
        break;
    case ContextMenuItemTagShowSpellingPanel:
        frame->editor()->showSpellingGuessPanel();
        break;
    case ContextMenuItemTagCheckSpelling:
        frame->editor()->advanceToNextMisspelling();
        break;
    case ContextMenuItemTagCheckSpellingWhileTyping:
        frame->editor()->toggleContinuousSpellChecking();
        break;
    case ContextMenuItemTagCheckGrammarWithSpelling:
        frame->editor()->toggleGrammarChecking();
        break;
    default:
        break;
    }
}

void ContextMenuController::appendItem(ContextMenuItem& menuItem, ContextMenu* parentMenu)
{
    checkOrEnableIfNeeded(menuItem);
    if (parentMenu)
        parentMenu->appendItem(menuItem);
}

void ContextMenuController::createAndAppendFontSubMenu(ContextMenuItem& fontMenuItem)
{
    ContextMenu fontMenu;

#if PLATFORM(MAC)
    ContextMenuItem showFonts(ActionType, ContextMenuItemTagShowFonts, contextMenuItemTagShowFonts());
#endif
    ContextMenuItem bold(CheckableActionType, ContextMenuItemTagBold, contextMenuItemTagBold());
    ContextMenuItem italic(CheckableActionType, ContextMenuItemTagItalic, contextMenuItemTagItalic());
    ContextMenuItem underline(CheckableActionType, ContextMenuItemTagUnderline, contextMenuItemTagUnderline());
    ContextMenuItem outline(ActionType, ContextMenuItemTagOutline, contextMenuItemTagOutline());
#if PLATFORM(MAC)
    ContextMenuItem styles(ActionType, ContextMenuItemTagStyles, contextMenuItemTagStyles());
    ContextMenuItem showColors(ActionType, ContextMenuItemTagShowColors, contextMenuItemTagShowColors());
#endif

#if PLATFORM(MAC)
    appendItem(showFonts, &fontMenu);
#endif
    appendItem(bold, &fontMenu);
    appendItem(italic, &fontMenu);
    appendItem(underline, &fontMenu);
    appendItem(outline, &fontMenu);
#if PLATFORM(MAC)
    appendItem(styles, &fontMenu);
    appendItem(*separatorItem(), &fontMenu);
    appendItem(showColors, &fontMenu);
#endif

    fontMenuItem.setSubMenu(&fontMenu);
}


#if !PLATFORM(GTK)

void ContextMenuController::createAndAppendSpellingAndGrammarSubMenu(ContextMenuItem& spellingAndGrammarMenuItem)
{
    ContextMenu spellingAndGrammarMenu;

    ContextMenuItem showSpellingPanel(ActionType, ContextMenuItemTagShowSpellingPanel,
        contextMenuItemTagShowSpellingPanel(true));
    ContextMenuItem checkSpelling(ActionType, ContextMenuItemTagCheckSpelling,
        contextMenuItemTagCheckSpelling());
    ContextMenuItem checkAsYouType(CheckableActionType, ContextMenuItemTagCheckSpellingWhileTyping,
        contextMenuItemTagCheckSpellingWhileTyping());
    ContextMenuItem grammarWithSpelling(CheckableActionType, ContextMenuItemTagCheckGrammarWithSpelling,
        contextMenuItemTagCheckGrammarWithSpelling());
#if PLATFORM(MAC) && !defined(BUILDING_ON_LEOPARD)
    ContextMenuItem correctSpelling(CheckableActionType, ContextMenuItemTagCorrectSpellingAutomatically,
        contextMenuItemTagCorrectSpellingAutomatically());
#endif

    appendItem(showSpellingPanel, &spellingAndGrammarMenu);
    appendItem(checkSpelling, &spellingAndGrammarMenu);
#if PLATFORM(MAC) && !defined(BUILDING_ON_LEOPARD)
    appendItem(*separatorItem(), &spellingAndGrammarMenu);
#endif
    appendItem(checkAsYouType, &spellingAndGrammarMenu);
    appendItem(grammarWithSpelling, &spellingAndGrammarMenu);
#if PLATFORM(MAC) && !defined(BUILDING_ON_LEOPARD)
    appendItem(correctSpelling, &spellingAndGrammarMenu);
#endif

    spellingAndGrammarMenuItem.setSubMenu(&spellingAndGrammarMenu);
}

#endif // !PLATFORM(GTK)


#if PLATFORM(MAC)

void ContextMenuController::createAndAppendSpeechSubMenu(ContextMenuItem& speechMenuItem)
{
    ContextMenu speechMenu;

    ContextMenuItem start(ActionType, ContextMenuItemTagStartSpeaking, contextMenuItemTagStartSpeaking());
    ContextMenuItem stop(ActionType, ContextMenuItemTagStopSpeaking, contextMenuItemTagStopSpeaking());

    appendItem(start, &speechMenu);
    appendItem(stop, &speechMenu);

    speechMenuItem.setSubMenu(&speechMenu);
}

#endif
 
#if !PLATFORM(GTK)

void ContextMenuController::createAndAppendWritingDirectionSubMenu(ContextMenuItem& writingDirectionMenuItem)
{
    ContextMenu writingDirectionMenu;

    ContextMenuItem defaultItem(ActionType, ContextMenuItemTagDefaultDirection, 
        contextMenuItemTagDefaultDirection());
    ContextMenuItem ltr(CheckableActionType, ContextMenuItemTagLeftToRight, contextMenuItemTagLeftToRight());
    ContextMenuItem rtl(CheckableActionType, ContextMenuItemTagRightToLeft, contextMenuItemTagRightToLeft());

    appendItem(defaultItem, &writingDirectionMenu);
    appendItem(ltr, &writingDirectionMenu);
    appendItem(rtl, &writingDirectionMenu);

    writingDirectionMenuItem.setSubMenu(&writingDirectionMenu);
}

void ContextMenuController::createAndAppendTextDirectionSubMenu(ContextMenuItem& textDirectionMenuItem)
{
    ContextMenu textDirectionMenu;

    ContextMenuItem defaultItem(ActionType, ContextMenuItemTagTextDirectionDefault, contextMenuItemTagDefaultDirection());
    ContextMenuItem ltr(CheckableActionType, ContextMenuItemTagTextDirectionLeftToRight, contextMenuItemTagLeftToRight());
    ContextMenuItem rtl(CheckableActionType, ContextMenuItemTagTextDirectionRightToLeft, contextMenuItemTagRightToLeft());

    appendItem(defaultItem, &textDirectionMenu);
    appendItem(ltr, &textDirectionMenu);
    appendItem(rtl, &textDirectionMenu);

    textDirectionMenuItem.setSubMenu(&textDirectionMenu);
}

#endif

#if PLATFORM(MAC) && !defined(BUILDING_ON_LEOPARD)

void ContextMenuController::createAndAppendSubstitutionsSubMenu(ContextMenuItem& substitutionsMenuItem)
{
    ContextMenu substitutionsMenu;

    ContextMenuItem showSubstitutions(ActionType, ContextMenuItemTagShowSubstitutions, contextMenuItemTagShowSubstitutions(true));
    ContextMenuItem smartCopyPaste(CheckableActionType, ContextMenuItemTagSmartCopyPaste, contextMenuItemTagSmartCopyPaste());
    ContextMenuItem smartQuotes(CheckableActionType, ContextMenuItemTagSmartQuotes, contextMenuItemTagSmartQuotes());
    ContextMenuItem smartDashes(CheckableActionType, ContextMenuItemTagSmartDashes, contextMenuItemTagSmartDashes());
    ContextMenuItem smartLinks(CheckableActionType, ContextMenuItemTagSmartLinks, contextMenuItemTagSmartLinks());
    ContextMenuItem textReplacement(CheckableActionType, ContextMenuItemTagTextReplacement, contextMenuItemTagTextReplacement());

    appendItem(showSubstitutions, &substitutionsMenu);
    appendItem(*separatorItem(), &substitutionsMenu);
    appendItem(smartCopyPaste, &substitutionsMenu);
    appendItem(smartQuotes, &substitutionsMenu);
    appendItem(smartDashes, &substitutionsMenu);
    appendItem(smartLinks, &substitutionsMenu);
    appendItem(textReplacement, &substitutionsMenu);

    substitutionsMenuItem.setSubMenu(&substitutionsMenu);
}

void ContextMenuController::createAndAppendTransformationsSubMenu(ContextMenuItem& transformationsMenuItem)
{
    ContextMenu transformationsMenu;

    ContextMenuItem makeUpperCase(ActionType, ContextMenuItemTagMakeUpperCase, contextMenuItemTagMakeUpperCase());
    ContextMenuItem makeLowerCase(ActionType, ContextMenuItemTagMakeLowerCase, contextMenuItemTagMakeLowerCase());
    ContextMenuItem capitalize(ActionType, ContextMenuItemTagCapitalize, contextMenuItemTagCapitalize());

    appendItem(makeUpperCase, &transformationsMenu);
    appendItem(makeLowerCase, &transformationsMenu);
    appendItem(capitalize, &transformationsMenu);

    transformationsMenuItem.setSubMenu(&transformationsMenu);
}

#endif

static bool selectionContainsPossibleWord(Frame* frame)
{
    // Current algorithm: look for a character that's not just a separator.
    for (TextIterator it(frame->selection()->toNormalizedRange().get()); !it.atEnd(); it.advance()) {
        int length = it.length();
        const UChar* characters = it.characters();
        for (int i = 0; i < length; ++i)
            if (!(category(characters[i]) & (Separator_Space | Separator_Line | Separator_Paragraph)))
                return true;
    }
    return false;
}

#if PLATFORM(MAC)
#if defined(BUILDING_ON_LEOPARD) || defined(BUILDING_ON_SNOW_LEOPARD)
#define INCLUDE_SPOTLIGHT_CONTEXT_MENU_ITEM 1
#else
#define INCLUDE_SPOTLIGHT_CONTEXT_MENU_ITEM 0
#endif
#endif

void ContextMenuController::checkOrEnableIfNeeded(ContextMenuItem& item) const
{
    if (item.type() == SeparatorType)
        return;
    
    Frame* frame = m_hitTestResult.innerNonSharedNode()->document()->frame();
    if (!frame)
        return;

    // Custom items already have proper checked and enabled values.
    if (ContextMenuItemBaseCustomTag <= item.action() && item.action() <= ContextMenuItemLastCustomTag)
        return;

    bool shouldEnable = true;
    bool shouldCheck = false; 

    switch (item.action()) {
        case ContextMenuItemTagCheckSpelling:
            shouldEnable = frame->editor()->canEdit();
            break;
        case ContextMenuItemTagDefaultDirection:
            shouldCheck = false;
            shouldEnable = false;
            break;
        case ContextMenuItemTagLeftToRight:
        case ContextMenuItemTagRightToLeft: {
            String direction = item.action() == ContextMenuItemTagLeftToRight ? "ltr" : "rtl";
            shouldCheck = frame->editor()->selectionHasStyle(CSSPropertyDirection, direction) != FalseTriState;
            shouldEnable = true;
            break;
        }
        case ContextMenuItemTagTextDirectionDefault: {
            Editor::Command command = frame->editor()->command("MakeTextWritingDirectionNatural");
            shouldCheck = command.state() == TrueTriState;
            shouldEnable = command.isEnabled();
            break;
        }
        case ContextMenuItemTagTextDirectionLeftToRight: {
            Editor::Command command = frame->editor()->command("MakeTextWritingDirectionLeftToRight");
            shouldCheck = command.state() == TrueTriState;
            shouldEnable = command.isEnabled();
            break;
        }
        case ContextMenuItemTagTextDirectionRightToLeft: {
            Editor::Command command = frame->editor()->command("MakeTextWritingDirectionRightToLeft");
            shouldCheck = command.state() == TrueTriState;
            shouldEnable = command.isEnabled();
            break;
        }
        case ContextMenuItemTagCopy:
            shouldEnable = frame->editor()->canDHTMLCopy() || frame->editor()->canCopy();
            break;
        case ContextMenuItemTagCut:
            shouldEnable = frame->editor()->canDHTMLCut() || frame->editor()->canCut();
            break;
        case ContextMenuItemTagIgnoreSpelling:
        case ContextMenuItemTagLearnSpelling:
            shouldEnable = frame->selection()->isRange();
            break;
        case ContextMenuItemTagPaste:
            shouldEnable = frame->editor()->canDHTMLPaste() || frame->editor()->canPaste();
            break;
#if PLATFORM(GTK)
        case ContextMenuItemTagDelete:
            shouldEnable = frame->editor()->canDelete();
            break;
        case ContextMenuItemTagInputMethods:
        case ContextMenuItemTagUnicode:
            shouldEnable = true;
            break;
#endif
#if PLATFORM(GTK) || PLATFORM(EFL)
        case ContextMenuItemTagSelectAll:
            shouldEnable = true;
            break;
#endif
        case ContextMenuItemTagUnderline: {
            shouldCheck = frame->editor()->selectionHasStyle(CSSPropertyWebkitTextDecorationsInEffect, "underline") != FalseTriState;
            shouldEnable = frame->editor()->canEditRichly();
            break;
        }
        case ContextMenuItemTagLookUpInDictionary:
            shouldEnable = frame->selection()->isRange();
            break;
        case ContextMenuItemTagCheckGrammarWithSpelling:
            if (frame->editor()->isGrammarCheckingEnabled())
                shouldCheck = true;
            shouldEnable = true;
            break;
        case ContextMenuItemTagItalic: {
            shouldCheck = frame->editor()->selectionHasStyle(CSSPropertyFontStyle, "italic") != FalseTriState;
            shouldEnable = frame->editor()->canEditRichly();
            break;
        }
        case ContextMenuItemTagBold: {
            shouldCheck = frame->editor()->selectionHasStyle(CSSPropertyFontWeight, "bold") != FalseTriState;
            shouldEnable = frame->editor()->canEditRichly();
            break;
        }
        case ContextMenuItemTagOutline:
            shouldEnable = false;
            break;
        case ContextMenuItemTagShowSpellingPanel:
            if (frame->editor()->spellingPanelIsShowing())
                item.setTitle(contextMenuItemTagShowSpellingPanel(false));
            else
                item.setTitle(contextMenuItemTagShowSpellingPanel(true));
            shouldEnable = frame->editor()->canEdit();
            break;
        case ContextMenuItemTagNoGuessesFound:
            shouldEnable = false;
            break;
        case ContextMenuItemTagCheckSpellingWhileTyping:
            shouldCheck = frame->editor()->isContinuousSpellCheckingEnabled();
            break;
#if PLATFORM(MAC)
        case ContextMenuItemTagSubstitutionsMenu:
        case ContextMenuItemTagTransformationsMenu:
            break;
        case ContextMenuItemTagShowSubstitutions:
#ifndef BUILDING_ON_LEOPARD
            if (frame->editor()->substitutionsPanelIsShowing())
                item.setTitle(contextMenuItemTagShowSubstitutions(false));
            else
                item.setTitle(contextMenuItemTagShowSubstitutions(true));
            shouldEnable = frame->editor()->canEdit();
#endif
            break;
        case ContextMenuItemTagMakeUpperCase:
        case ContextMenuItemTagMakeLowerCase:
        case ContextMenuItemTagCapitalize:
        case ContextMenuItemTagChangeBack:
            shouldEnable = frame->editor()->canEdit();
            break;
        case ContextMenuItemTagCorrectSpellingAutomatically:
#ifndef BUILDING_ON_LEOPARD
            shouldCheck = frame->editor()->isAutomaticSpellingCorrectionEnabled();
#endif
            break;
        case ContextMenuItemTagSmartCopyPaste:
#ifndef BUILDING_ON_LEOPARD
            shouldCheck = frame->editor()->smartInsertDeleteEnabled();
#endif
            break;
        case ContextMenuItemTagSmartQuotes:
#ifndef BUILDING_ON_LEOPARD
            shouldCheck = frame->editor()->isAutomaticQuoteSubstitutionEnabled();
#endif
            break;
        case ContextMenuItemTagSmartDashes:
#ifndef BUILDING_ON_LEOPARD
            shouldCheck = frame->editor()->isAutomaticDashSubstitutionEnabled();
#endif
            break;
        case ContextMenuItemTagSmartLinks:
#ifndef BUILDING_ON_LEOPARD
            shouldCheck = frame->editor()->isAutomaticLinkDetectionEnabled();
#endif
            break;
        case ContextMenuItemTagTextReplacement:
#ifndef BUILDING_ON_LEOPARD
            shouldCheck = frame->editor()->isAutomaticTextReplacementEnabled();
#endif
            break;
        case ContextMenuItemTagStopSpeaking:
            shouldEnable = client() && client()->isSpeaking();
            break;
#else // PLATFORM(MAC) ends here
        case ContextMenuItemTagStopSpeaking:
            break;
#endif
#if PLATFORM(GTK)
        case ContextMenuItemTagGoBack:
            shouldEnable = frame->page() && frame->page()->backForward()->canGoBackOrForward(-1);
            break;
        case ContextMenuItemTagGoForward:
            shouldEnable = frame->page() && frame->page()->backForward()->canGoBackOrForward(1);
            break;
        case ContextMenuItemTagStop:
            shouldEnable = frame->loader()->documentLoader()->isLoadingInAPISense();
            break;
        case ContextMenuItemTagReload:
            shouldEnable = !frame->loader()->documentLoader()->isLoadingInAPISense();
            break;
        case ContextMenuItemTagFontMenu:
            shouldEnable = frame->editor()->canEditRichly();
            break;
#else
        case ContextMenuItemTagGoBack:
        case ContextMenuItemTagGoForward:
        case ContextMenuItemTagStop:
        case ContextMenuItemTagReload:
        case ContextMenuItemTagFontMenu:
#endif
        case ContextMenuItemTagNoAction:
        case ContextMenuItemTagOpenLinkInNewWindow:
        case ContextMenuItemTagDownloadLinkToDisk:
        case ContextMenuItemTagCopyLinkToClipboard:
        case ContextMenuItemTagOpenImageInNewWindow:
        case ContextMenuItemTagDownloadImageToDisk:
        case ContextMenuItemTagCopyImageToClipboard:
#if PLATFORM(QT) || PLATFORM(GTK) || PLATFORM(EFL)
        case ContextMenuItemTagCopyImageUrlToClipboard:
#endif
            break;
        case ContextMenuItemTagOpenMediaInNewWindow:
            if (m_hitTestResult.mediaIsVideo())
                item.setTitle(contextMenuItemTagOpenVideoInNewWindow());
            else
                item.setTitle(contextMenuItemTagOpenAudioInNewWindow());
            break;
        case ContextMenuItemTagCopyMediaLinkToClipboard:
            if (m_hitTestResult.mediaIsVideo())
                item.setTitle(contextMenuItemTagCopyVideoLinkToClipboard());
            else
                item.setTitle(contextMenuItemTagCopyAudioLinkToClipboard());
            break;
        case ContextMenuItemTagToggleMediaControls:
            shouldCheck = m_hitTestResult.mediaControlsEnabled();
            break;
        case ContextMenuItemTagToggleMediaLoop:
            shouldCheck = m_hitTestResult.mediaLoopEnabled();
            break;
        case ContextMenuItemTagEnterVideoFullscreen:
            shouldEnable = m_hitTestResult.mediaSupportsFullscreen();
            break;
        case ContextMenuItemTagOpenFrameInNewWindow:
        case ContextMenuItemTagSpellingGuess:
        case ContextMenuItemTagOther:
        case ContextMenuItemTagSearchInSpotlight:
        case ContextMenuItemTagSearchWeb:
        case ContextMenuItemTagOpenWithDefaultApplication:
        case ContextMenuItemPDFActualSize:
        case ContextMenuItemPDFZoomIn:
        case ContextMenuItemPDFZoomOut:
        case ContextMenuItemPDFAutoSize:
        case ContextMenuItemPDFSinglePage:
        case ContextMenuItemPDFFacingPages:
        case ContextMenuItemPDFContinuous:
        case ContextMenuItemPDFNextPage:
        case ContextMenuItemPDFPreviousPage:
        case ContextMenuItemTagOpenLink:
        case ContextMenuItemTagIgnoreGrammar:
        case ContextMenuItemTagSpellingMenu:
        case ContextMenuItemTagShowFonts:
        case ContextMenuItemTagStyles:
        case ContextMenuItemTagShowColors:
        case ContextMenuItemTagSpeechMenu:
        case ContextMenuItemTagStartSpeaking:
        case ContextMenuItemTagWritingDirectionMenu:
        case ContextMenuItemTagTextDirectionMenu:
        case ContextMenuItemTagPDFSinglePageScrolling:
        case ContextMenuItemTagPDFFacingPagesScrolling:
#if ENABLE(INSPECTOR)
        case ContextMenuItemTagInspectElement:
#endif
        case ContextMenuItemBaseCustomTag:
        case ContextMenuItemCustomTagNoAction:
        case ContextMenuItemLastCustomTag:
        case ContextMenuItemBaseApplicationTag:
            break;
        case ContextMenuItemTagMediaPlayPause:
            if (m_hitTestResult.mediaPlaying())
                item.setTitle(contextMenuItemTagMediaPause());
            else
                item.setTitle(contextMenuItemTagMediaPlay());
            break;
        case ContextMenuItemTagMediaMute:
            shouldEnable = m_hitTestResult.mediaHasAudio();
            shouldCheck = shouldEnable &&  m_hitTestResult.mediaMuted();
            break;
    }

    item.setChecked(shouldCheck);
    item.setEnabled(shouldEnable);
}

void ContextMenuController::populate()
{
    ContextMenuItem OpenLinkItem(ActionType, ContextMenuItemTagOpenLink, contextMenuItemTagOpenLink());
    ContextMenuItem OpenLinkInNewWindowItem(ActionType, ContextMenuItemTagOpenLinkInNewWindow,
            contextMenuItemTagOpenLinkInNewWindow());
    ContextMenuItem DownloadFileItem(ActionType, ContextMenuItemTagDownloadLinkToDisk,
            contextMenuItemTagDownloadLinkToDisk());
    ContextMenuItem CopyLinkItem(ActionType, ContextMenuItemTagCopyLinkToClipboard,
            contextMenuItemTagCopyLinkToClipboard());
    ContextMenuItem OpenImageInNewWindowItem(ActionType, ContextMenuItemTagOpenImageInNewWindow,
            contextMenuItemTagOpenImageInNewWindow());
    ContextMenuItem DownloadImageItem(ActionType, ContextMenuItemTagDownloadImageToDisk,
            contextMenuItemTagDownloadImageToDisk());
    ContextMenuItem CopyImageItem(ActionType, ContextMenuItemTagCopyImageToClipboard,
            contextMenuItemTagCopyImageToClipboard());
    ContextMenuItem CopyImageUrlItem(ActionType, ContextMenuItemTagCopyImageUrlToClipboard,
            contextMenuItemTagCopyImageUrlToClipboard());
    ContextMenuItem OpenMediaInNewWindowItem(ActionType, ContextMenuItemTagOpenMediaInNewWindow, String());
    ContextMenuItem CopyMediaLinkItem(ActionType, ContextMenuItemTagCopyMediaLinkToClipboard,
            String());
    ContextMenuItem MediaPlayPause(ActionType, ContextMenuItemTagMediaPlayPause,
            contextMenuItemTagMediaPlay());
    ContextMenuItem MediaMute(ActionType, ContextMenuItemTagMediaMute,
            contextMenuItemTagMediaMute());
    ContextMenuItem ToggleMediaControls(CheckableActionType, ContextMenuItemTagToggleMediaControls,
            contextMenuItemTagToggleMediaControls());
    ContextMenuItem ToggleMediaLoop(CheckableActionType, ContextMenuItemTagToggleMediaLoop,
            contextMenuItemTagToggleMediaLoop());
    ContextMenuItem EnterVideoFullscreen(ActionType, ContextMenuItemTagEnterVideoFullscreen,
            contextMenuItemTagEnterVideoFullscreen());
    ContextMenuItem CopyItem(ActionType, ContextMenuItemTagCopy, contextMenuItemTagCopy());
    ContextMenuItem BackItem(ActionType, ContextMenuItemTagGoBack, contextMenuItemTagGoBack());
    ContextMenuItem ForwardItem(ActionType, ContextMenuItemTagGoForward,  contextMenuItemTagGoForward());
    ContextMenuItem StopItem(ActionType, ContextMenuItemTagStop, contextMenuItemTagStop());
    ContextMenuItem ReloadItem(ActionType, ContextMenuItemTagReload, contextMenuItemTagReload());
    ContextMenuItem OpenFrameItem(ActionType, ContextMenuItemTagOpenFrameInNewWindow,
            contextMenuItemTagOpenFrameInNewWindow());
    ContextMenuItem NoGuessesItem(ActionType, ContextMenuItemTagNoGuessesFound,
            contextMenuItemTagNoGuessesFound());
    ContextMenuItem IgnoreSpellingItem(ActionType, ContextMenuItemTagIgnoreSpelling,
            contextMenuItemTagIgnoreSpelling());
    ContextMenuItem LearnSpellingItem(ActionType, ContextMenuItemTagLearnSpelling,
            contextMenuItemTagLearnSpelling());
    ContextMenuItem IgnoreGrammarItem(ActionType, ContextMenuItemTagIgnoreGrammar,
            contextMenuItemTagIgnoreGrammar());
    ContextMenuItem CutItem(ActionType, ContextMenuItemTagCut, contextMenuItemTagCut());
    ContextMenuItem PasteItem(ActionType, ContextMenuItemTagPaste, contextMenuItemTagPaste());
    ContextMenuItem DeleteItem(ActionType, ContextMenuItemTagDelete, contextMenuItemTagDelete());
    ContextMenuItem SelectAllItem(ActionType, ContextMenuItemTagSelectAll, contextMenuItemTagSelectAll());

    Node* node = m_hitTestResult.innerNonSharedNode();
    if (!node)
        return;
    if (!m_hitTestResult.isContentEditable() && (node->isElementNode() && static_cast<Element*>(node)->isFormControlElement()))
        return;
    Frame* frame = node->document()->frame();
    if (!frame)
        return;

    if (!m_hitTestResult.isContentEditable()) {
        FrameLoader* loader = frame->loader();
        KURL linkURL = m_hitTestResult.absoluteLinkURL();
        if (!linkURL.isEmpty()) {
            if (loader->client()->canHandleRequest(ResourceRequest(linkURL))) {
                appendItem(OpenLinkItem, m_contextMenu.get());
                appendItem(OpenLinkInNewWindowItem, m_contextMenu.get());
                appendItem(DownloadFileItem, m_contextMenu.get());
            }
            appendItem(CopyLinkItem, m_contextMenu.get());
        }

        KURL imageURL = m_hitTestResult.absoluteImageURL();
        if (!imageURL.isEmpty()) {
            if (!linkURL.isEmpty())
                appendItem(*separatorItem(), m_contextMenu.get());

            appendItem(OpenImageInNewWindowItem, m_contextMenu.get());
            appendItem(DownloadImageItem, m_contextMenu.get());
            if (imageURL.isLocalFile() || m_hitTestResult.image())
                appendItem(CopyImageItem, m_contextMenu.get());
            appendItem(CopyImageUrlItem, m_contextMenu.get());
        }

        KURL mediaURL = m_hitTestResult.absoluteMediaURL();
        if (!mediaURL.isEmpty()) {
            if (!linkURL.isEmpty() || !imageURL.isEmpty())
                appendItem(*separatorItem(), m_contextMenu.get());

            appendItem(MediaPlayPause, m_contextMenu.get());
            appendItem(MediaMute, m_contextMenu.get());
            appendItem(ToggleMediaControls, m_contextMenu.get());
            appendItem(ToggleMediaLoop, m_contextMenu.get());
            appendItem(EnterVideoFullscreen, m_contextMenu.get());

            appendItem(*separatorItem(), m_contextMenu.get());
            appendItem(CopyMediaLinkItem, m_contextMenu.get());
            appendItem(OpenMediaInNewWindowItem, m_contextMenu.get());
        }

        if (imageURL.isEmpty() && linkURL.isEmpty() && mediaURL.isEmpty()) {
            if (m_hitTestResult.isSelected()) {
                appendItem(CopyItem, m_contextMenu.get());
            } else {
                appendItem(BackItem, m_contextMenu.get());
                appendItem(ForwardItem, m_contextMenu.get());
                appendItem(StopItem, m_contextMenu.get());
                appendItem(ReloadItem, m_contextMenu.get());

                if (frame->page() && frame != frame->page()->mainFrame())
                    appendItem(OpenFrameItem, m_contextMenu.get());
            }
        }
    } else { // Make an editing context menu
        FrameSelection* selection = frame->selection();
        bool inPasswordField = selection->isInPasswordField();
        bool spellCheckingEnabled = frame->editor()->isSpellCheckingEnabledFor(node);

        if (!inPasswordField && spellCheckingEnabled) {
            // Consider adding spelling-related or grammar-related context menu items (never both, since a single selected range
            // is never considered a misspelling and bad grammar at the same time)
            bool misspelling;
            bool badGrammar;
            Vector<String> guesses = frame->editor()->guessesForMisspelledOrUngrammaticalSelection(misspelling, badGrammar);
            if (misspelling || badGrammar) {
                size_t size = guesses.size();
                if (size == 0) {
                    // If there's bad grammar but no suggestions (e.g., repeated word), just leave off the suggestions
                    // list and trailing separator rather than adding a "No Guesses Found" item (matches AppKit)
                    if (misspelling) {
                        appendItem(NoGuessesItem, m_contextMenu.get());
                        appendItem(*separatorItem(), m_contextMenu.get());
                    }
                } else {
                    for (unsigned i = 0; i < size; i++) {
                        const String &guess = guesses[i];
                        if (!guess.isEmpty()) {
                            ContextMenuItem item(ActionType, ContextMenuItemTagSpellingGuess, guess);
                            appendItem(item, m_contextMenu.get());
                        }
                    }
                    appendItem(*separatorItem(), m_contextMenu.get());
                }

                if (misspelling) {
                    appendItem(IgnoreSpellingItem, m_contextMenu.get());
                    appendItem(LearnSpellingItem, m_contextMenu.get());
                } else
                    appendItem(IgnoreGrammarItem, m_contextMenu.get());
                appendItem(*separatorItem(), m_contextMenu.get());
            }
        }

        FrameLoader* loader = frame->loader();
        KURL linkURL = m_hitTestResult.absoluteLinkURL();
        if (!linkURL.isEmpty()) {
            if (loader->client()->canHandleRequest(ResourceRequest(linkURL))) {
                appendItem(OpenLinkItem, m_contextMenu.get());
                appendItem(OpenLinkInNewWindowItem, m_contextMenu.get());
                appendItem(DownloadFileItem, m_contextMenu.get());
            }
            appendItem(CopyLinkItem, m_contextMenu.get());
            appendItem(*separatorItem(), m_contextMenu.get());
        }

        appendItem(CutItem, m_contextMenu.get());
        appendItem(CopyItem, m_contextMenu.get());
        appendItem(PasteItem, m_contextMenu.get());
        appendItem(DeleteItem, m_contextMenu.get());
        appendItem(*separatorItem(), m_contextMenu.get());
        appendItem(SelectAllItem, m_contextMenu.get());

        if (!inPasswordField) {
            bool shouldShowFontMenu = frame->editor()->canEditRichly();
            if (shouldShowFontMenu) {
                ContextMenuItem FontMenuItem(SubmenuType, ContextMenuItemTagFontMenu,
                        contextMenuItemTagFontMenu());
                createAndAppendFontSubMenu(FontMenuItem);
                appendItem(FontMenuItem, m_contextMenu.get());
            }
        }
    }
}


} // namespace WebCore

#endif // ENABLE(CONTEXT_MENUS)

