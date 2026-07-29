/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2026 Usedesktop Authors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include <kicadgym/native_action_logger.h>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#include <wx/app.h>
#include <wx/aui/auibook.h>
#include <wx/checklst.h>
#include <wx/control.h>
#include <wx/dataview.h>
#include <wx/event.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/grid.h>
#include <wx/menu.h>
#include <wx/notebook.h>
#include <wx/propgrid/propgrid.h>
#include <wx/file.h>
#include <wx/tglbtn.h>
#include <wx/treectrl.h>
#include <wx/utils.h>
#include <wx/window.h>

namespace
{
std::atomic<uint64_t> s_eventId{ 0 };
std::atomic<uint64_t> s_interactionId{ 0 };
std::mutex            s_stateMutex;
uint64_t              s_pendingUiInteraction = 0;
uint64_t              s_pendingTransactionParent = 0;
std::vector<uint64_t> s_commandStack;

struct UI_ACTION
{
    std::string catalogId;
    std::string instanceId;
    std::string kind;
    std::string label;
    std::string controlType;
    std::string commandId;
    std::string semanticActionId;
    bool        enabled = true;
    wxRect      bounds;
};

std::string toUtf8( const wxString& aValue )
{
    const wxScopedCharBuffer buffer = aValue.ToUTF8();
    return buffer.data() ? std::string( buffer.data(), buffer.length() ) : std::string();
}

std::string escapeJson( const std::string& aValue )
{
    std::ostringstream out;
    for( const unsigned char value : aValue )
    {
        switch( value )
        {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if( value < 0x20 )
            {
                static const char* digits = "0123456789abcdef";
                out << "\\u00" << digits[value >> 4] << digits[value & 0x0f];
            }
            else
            {
                out << static_cast<char>( value );
            }
        }
    }
    return out.str();
}

std::string quoted( const std::string& aValue )
{
    return "\"" + escapeJson( aValue ) + "\"";
}

std::string sanitizeIdPart( std::string aValue )
{
    std::string result;
    bool        separator = false;
    for( unsigned char value : aValue )
    {
        if( std::isalnum( value ) )
        {
            result.push_back( static_cast<char>( std::tolower( value ) ) );
            separator = false;
        }
        else if( !separator && !result.empty() )
        {
            result.push_back( '_' );
            separator = true;
        }
    }
    while( !result.empty() && result.back() == '_' )
        result.pop_back();
    return result.empty() ? "unnamed" : result;
}

std::string semanticActionId( const std::string& aCommandId, const std::string& aLabel,
                              const std::string& aKind, const std::string& aEditor = {} )
{
    return KICADGYM::ClassifyNativeSemanticAction( aCommandId, aLabel, aKind, aEditor );
}

std::string verifierIdsJson( const std::string& aActionId, const std::string& aCatalogId,
                             const std::string& aCommandId,
                             const std::string& aPropertyOwner,
                             const std::string& aPropertyName )
{
    std::vector<std::string> ids = {
        "catalog.known:" + aCatalogId,
        "ui.visible:" + aActionId,
        "ui.enabled:" + aActionId,
        "ui.invoked:" + aActionId,
    };
    if( !aCommandId.empty() )
        ids.push_back( "operator.invoked:" + aCommandId );
    if( !aPropertyOwner.empty() && !aPropertyName.empty() )
        ids.push_back( "property.updated:" + aPropertyOwner + "." + aPropertyName );
    std::ostringstream out;
    out << "[";
    for( size_t i = 0; i < ids.size(); ++i )
    {
        if( i )
            out << ",";
        out << quoted( ids[i] );
    }
    out << "]";
    return out.str();
}

std::string verifierBindingJson( const std::string& aPrimitive,
                                 const std::string& aVerifierId,
                                 const std::string& aScope,
                                 const std::string& aPhase,
                                 bool aRequired )
{
    std::ostringstream out;
    out << "{\"primitive\":" << quoted( aPrimitive )
        << ",\"verifier_id\":" << quoted( aVerifierId )
        << ",\"scope\":" << quoted( aScope )
        << ",\"phase\":" << quoted( aPhase )
        << ",\"kind\":\"boolean\",\"required\":" << ( aRequired ? "true" : "false" )
        << "}";
    return out.str();
}

std::string verifierBindingsJson( const std::string& aActionId,
                                  const std::string& aCatalogId,
                                  const std::string& aCommandId,
                                  const std::string& aPropertyOwner,
                                  const std::string& aPropertyName )
{
    std::vector<std::string> bindings = {
        verifierBindingJson( "catalog.known", "catalog.known:" + aCatalogId, "catalog", "precondition", true ),
        verifierBindingJson( "ui.visible", "ui.visible:" + aActionId, "recording", "recording_precondition", false ),
        verifierBindingJson( "ui.enabled", "ui.enabled:" + aActionId, "recording", "recording_precondition", false ),
        verifierBindingJson( "ui.hit_bbox", "ui.hit_bbox:" + aActionId, "recording", "recording_evidence", false ),
        verifierBindingJson( "ui.invoked", "ui.invoked:" + aActionId, "recording", "recording_evidence", false ),
    };
    if( !aCommandId.empty() )
        bindings.push_back( verifierBindingJson( "operator.invoked", "operator.invoked:" + aCommandId,
                                                  "command", "postcondition", true ) );
    if( !aPropertyOwner.empty() && !aPropertyName.empty() )
        bindings.push_back( verifierBindingJson( "property.updated",
                                                  "property.updated:" + aPropertyOwner + "." + aPropertyName,
                                                  "property", "postcondition", true ) );
    std::ostringstream out;
    out << "[";
    for( size_t i = 0; i < bindings.size(); ++i )
    {
        if( i )
            out << ",";
        out << bindings[i];
    }
    out << "]";
    return out.str();
}

int64_t epochTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch() )
            .count();
}

uint64_t nextInteractionId()
{
    return ++s_interactionId;
}

bool nativeActionTarget( wxString* aTarget )
{
    wxString controlFile;
    if( !wxGetEnv( wxS( "RL_ENV_NATIVE_ACTION_CONTROL_FILE" ), &controlFile )
        || controlFile.empty() )
    {
        return false;
    }

    wxFFile file( controlFile, wxS( "rb" ) );
    if( !file.IsOpened() || !file.ReadAll( aTarget ) )
        return false;

    aTarget->Trim( true ).Trim( false );
    return !aTarget->empty();
}

std::string windowLabel( wxWindow* aWindow )
{
    if( !aWindow )
        return {};
    wxString label = aWindow->GetLabel();
    if( label.empty() )
        label = aWindow->GetName();
    return toUtf8( label );
}

std::string windowPath( wxWindow* aWindow )
{
    std::vector<std::string> parts;
    for( wxWindow* current = aWindow; current; current = current->GetParent() )
    {
        const std::string className = toUtf8( current->GetClassInfo()->GetClassName() );
        const std::string name = toUtf8( current->GetName() );
        const std::string label = windowLabel( current );
        std::string part = !name.empty() && name != "window" ? name
                          : !label.empty() ? className + "_" + label
                                           : className;
        size_t ordinal = 0;
        if( wxWindow* parent = current->GetParent() )
        {
            for( wxWindow* sibling : parent->GetChildren() )
            {
                if( sibling == current )
                    break;
                if( toUtf8( sibling->GetClassInfo()->GetClassName() ) == className
                    && windowLabel( sibling ) == label )
                {
                    ++ordinal;
                }
            }
        }
        part += "_" + std::to_string( ordinal );
        parts.push_back( sanitizeIdPart( part ) );
    }
    std::reverse( parts.begin(), parts.end() );
    std::ostringstream out;
    for( size_t i = 0; i < parts.size(); ++i )
    {
        if( i )
            out << ".";
        out << parts[i];
    }
    return out.str();
}

std::string controlType( wxWindow* aWindow )
{
    if( !aWindow )
        return "control";
    std::string type = toUtf8( aWindow->GetClassInfo()->GetClassName() );
    std::transform( type.begin(), type.end(), type.begin(),
                    []( unsigned char value ) { return static_cast<char>( std::tolower( value ) ); } );
    if( type.find( "propertygrid" ) != std::string::npos ) return "property_grid";
    if( type.find( "grid" ) != std::string::npos ) return "grid";
    if( type.find( "tree" ) != std::string::npos ) return "tree";
    if( type.find( "dataview" ) != std::string::npos ) return "data_view";
    if( type.find( "notebook" ) != std::string::npos ) return "tab";
    if( type.find( "text" ) != std::string::npos ) return "text_input";
    if( type.find( "slider" ) != std::string::npos || type.find( "spin" ) != std::string::npos ) return "numeric_input";
    if( type.find( "combo" ) != std::string::npos || type.find( "choice" ) != std::string::npos ) return "choice";
    if( type.find( "check" ) != std::string::npos || type.find( "radio" ) != std::string::npos ) return "toggle";
    if( type.find( "button" ) != std::string::npos ) return "button";
    return "control";
}

bool isActionableWindow( wxWindow* aWindow )
{
    if( !aWindow || !aWindow->IsShownOnScreen() )
        return false;
    const std::string type = controlType( aWindow );
    return type != "control" || dynamic_cast<wxControl*>( aWindow ) != nullptr;
}

UI_ACTION actionForWindow( wxWindow* aWindow )
{
    UI_ACTION action;
    action.label = windowLabel( aWindow );
    action.controlType = controlType( aWindow );
    const std::string owner = sanitizeIdPart( toUtf8( aWindow->GetClassInfo()->GetClassName() ) );
    const std::string name = toUtf8( aWindow->GetName() );
    const std::string identity = !name.empty() && name != "window"
                                         ? name
                                         : owner + "_" + action.label;
    action.catalogId = "kicad.ui." + owner + "." + sanitizeIdPart( identity );
    action.instanceId = "kicad.instance." + windowPath( aWindow ) + "." + sanitizeIdPart( action.catalogId );
    action.kind = action.controlType == "button" ? "control" : "property";
    wxWindow* topLevel = wxGetTopLevelParent( aWindow );
    const std::string editor = topLevel ? windowLabel( topLevel ) : std::string();
    action.semanticActionId = semanticActionId( "", action.label, action.controlType, editor );
    action.enabled = aWindow->IsEnabled();
    action.bounds = aWindow->GetScreenRect();
    return action;
}

void collectWindowActions( wxWindow* aWindow, std::vector<UI_ACTION>& aActions,
                           std::set<std::string>& aSeen )
{
    if( isActionableWindow( aWindow ) )
    {
        UI_ACTION action = actionForWindow( aWindow );
        if( aSeen.insert( action.instanceId ).second )
            aActions.push_back( std::move( action ) );
    }
    for( wxWindow* child : aWindow->GetChildren() )
        collectWindowActions( child, aActions, aSeen );
}

void collectMenuActions( wxMenu* aMenu, const std::string& aParentPath,
                         std::vector<UI_ACTION>& aActions, std::set<std::string>& aSeen )
{
    if( !aMenu )
        return;
    for( wxMenuItemList::compatibility_iterator node = aMenu->GetMenuItems().GetFirst(); node;
         node = node->GetNext() )
    {
        wxMenuItem* item = node->GetData();
        if( !item || item->IsSeparator() )
            continue;
        const std::string label = toUtf8( item->GetItemLabelText() );
        const std::string path = aParentPath.empty() ? label : aParentPath + "." + label;
        if( item->GetSubMenu() )
        {
            collectMenuActions( item->GetSubMenu(), path, aActions, aSeen );
            continue;
        }
        UI_ACTION action;
        action.label = label;
        action.controlType = "menu";
        action.kind = "control";
        const std::string menuIdentity = item->GetId() == wxID_ANY
                                                 ? sanitizeIdPart( path )
                                                 : "command_" + std::to_string( item->GetId() );
        action.catalogId = "kicad.menu." + menuIdentity;
        action.instanceId = "kicad.instance.menu." + menuIdentity;
        action.commandId = "wx.menu." + menuIdentity;
        action.semanticActionId = semanticActionId( action.commandId, label, "menu" );
        action.enabled = item->IsEnabled();
        if( aSeen.insert( action.instanceId ).second )
            aActions.push_back( std::move( action ) );
    }
}

std::string actionJson( const UI_ACTION& aAction )
{
    std::ostringstream out;
    out << "{\"catalog_id\":" << quoted( aAction.catalogId )
        << ",\"instance_id\":" << quoted( aAction.instanceId )
        << ",\"kind\":" << quoted( aAction.kind )
        << ",\"label\":" << quoted( aAction.label )
        << ",\"button_type\":" << quoted( aAction.controlType )
        << ",\"visible\":true,\"enabled\":" << ( aAction.enabled ? "true" : "false" )
        << ",\"command_id\":" << quoted( aAction.commandId )
        << ",\"semantic_action_id\":" << quoted( aAction.semanticActionId )
        << ",\"verifier_ids\":"
        << verifierIdsJson( aAction.instanceId, aAction.catalogId, aAction.commandId, "", "" )
        << ",\"catalog_verifier_ids\":"
        << verifierIdsJson( aAction.catalogId, aAction.catalogId, aAction.commandId, "", "" )
        << ",\"verifier_bindings\":"
        << verifierBindingsJson( aAction.instanceId, aAction.catalogId, aAction.commandId, "", "" )
        << ",\"catalog_verifier_bindings\":"
        << verifierBindingsJson( aAction.catalogId, aAction.catalogId, aAction.commandId, "", "" )
        << ",\"bbox_win_px\":{\"xmin\":" << aAction.bounds.GetLeft()
        << ",\"ymin\":" << aAction.bounds.GetTop()
        << ",\"xmax\":" << aAction.bounds.GetRight()
        << ",\"ymax\":" << aAction.bounds.GetBottom() << "}}";
    return out.str();
}

void writeVisibleActionCatalog()
{
    wxString actionTarget;
    if( !nativeActionTarget( &actionTarget ) )
        return;
    std::vector<UI_ACTION> actions;
    std::set<std::string>  seen;
    for( wxWindowList::compatibility_iterator node = wxTopLevelWindows.GetFirst(); node;
         node = node->GetNext() )
    {
        wxWindow* window = node->GetData();
        collectWindowActions( window, actions, seen );
        if( auto* frame = dynamic_cast<wxFrame*>( window ); frame && frame->GetMenuBar() )
        {
            for( size_t i = 0; i < frame->GetMenuBar()->GetMenuCount(); ++i )
            {
                collectMenuActions( frame->GetMenuBar()->GetMenu( i ),
                                    toUtf8( frame->GetMenuBar()->GetMenuLabelText( i ) ), actions, seen );
            }
        }
    }
    wxString sessionId;
    wxGetEnv( wxS( "RL_ENV_SESSION_ID" ), &sessionId );
    std::ostringstream out;
    out << "{\"schema\":\"kicadgym.ui_state.v1\",\"session_id\":"
        << quoted( toUtf8( sessionId ) ) << ",\"captured_at_epoch_ms\":" << epochTimeMs()
        << ",\"visible_action_count\":" << actions.size() << ",\"visible_actions\":[";
    for( size_t i = 0; i < actions.size(); ++i )
    {
        if( i )
            out << ",";
        out << actionJson( actions[i] );
    }
    out << "]}";
    wxFileName stateFile( actionTarget );
    stateFile.SetFullName( wxS( "ui-state.json" ) );
    wxTempFile output( stateFile.GetFullPath() );
    if( output.IsOpened() )
    {
        const std::string payload = out.str();
        output.Write( wxString::FromUTF8( payload.c_str() ) );
        output.Commit();
    }
}

void appendEvent( uint64_t aInteractionId, uint64_t aParentInteractionId,
                  const std::string& aPhase, const std::string& aSource,
                  const std::string& aKind, const std::string& aLabel,
                  const std::string& aCommandId, const std::string& aPropertyOwner,
                  const std::string& aPropertyName, bool aCommandCompleted,
                  bool aPropertyUpdated, const std::string& aEditor,
                  const wxRect* aBounds )
{
    wxString target;
    if( !nativeActionTarget( &target ) )
        return;

    wxString sessionId;
    wxString envKind;
    wxGetEnv( wxS( "RL_ENV_SESSION_ID" ), &sessionId );
    wxGetEnv( wxS( "RL_ENV_KIND" ), &envKind );

    std::string catalogId;
    if( !aCommandId.empty() )
        catalogId = "kicad.command." + sanitizeIdPart( aCommandId );
    else if( !aPropertyOwner.empty() && !aPropertyName.empty() )
        catalogId = "kicad.property." + sanitizeIdPart( aPropertyOwner ) + "."
                    + sanitizeIdPart( aPropertyName );
    else
        catalogId = "kicad.ui." + sanitizeIdPart( aKind ) + "." + sanitizeIdPart( aLabel );
    const std::string instanceId = catalogId;
    const std::string semanticId = semanticActionId( aCommandId, aLabel, aKind, aEditor );

    std::ostringstream out;
    out << "{"
        << "\"schema\":\"rl_env.native_action.v1\","
        << "\"env_kind\":" << quoted( envKind.empty() ? "kicadgym" : toUtf8( envKind ) ) << ","
        << "\"application\":\"KiCad\","
        << "\"session_id\":" << quoted( toUtf8( sessionId ) ) << ","
        << "\"event_id\":" << ++s_eventId << ","
        << "\"interaction_id\":" << aInteractionId << ","
        << "\"parent_interaction_id\":" << aParentInteractionId << ","
        << "\"timestamp_epoch_ms\":" << epochTimeMs() << ","
        << "\"phase\":" << quoted( aPhase ) << ","
        << "\"source\":" << quoted( aSource ) << ","
        << "\"catalog_id\":" << quoted( catalogId ) << ","
        << "\"instance_id\":" << quoted( instanceId ) << ","
        << "\"kind\":" << quoted( aKind ) << ","
        << "\"label\":" << quoted( aLabel ) << ","
        << "\"button_type\":" << quoted( aKind ) << ","
        << "\"enabled\":true,"
        << "\"command_id\":" << quoted( aCommandId ) << ","
        << "\"command_expression\":\"\","
        << "\"property_owner\":" << quoted( aPropertyOwner ) << ","
        << "\"property_name\":" << quoted( aPropertyName ) << ","
        << "\"property_index\":-1,"
        << "\"command_completed\":" << ( aCommandCompleted ? "true" : "false" ) << ","
        << "\"property_updated\":" << ( aPropertyUpdated ? "true" : "false" ) << ","
        << "\"workspace\":\"\",\"screen\":\"\","
        << "\"editor\":" << quoted( aEditor ) << ",\"editor_mode\":\"\","
        << "\"target_type\":\"\",\"target_name\":\"\","
        << "\"semantic_action_id\":" << quoted( semanticId ) << ","
        << "\"verifier_ids\":"
        << verifierIdsJson( instanceId, catalogId, aCommandId, aPropertyOwner, aPropertyName ) << ","
        << "\"catalog_verifier_ids\":"
        << verifierIdsJson( catalogId, catalogId, aCommandId, aPropertyOwner, aPropertyName ) << ","
        << "\"verifier_bindings\":"
        << verifierBindingsJson( instanceId, catalogId, aCommandId, aPropertyOwner, aPropertyName ) << ","
        << "\"catalog_verifier_bindings\":"
        << verifierBindingsJson( catalogId, catalogId, aCommandId, aPropertyOwner, aPropertyName ) << ","
        << "\"ui_context\":{},\"bbox_win_px\":{";

    if( aBounds )
    {
        out << "\"xmin\":" << aBounds->GetLeft() << ","
            << "\"ymin\":" << aBounds->GetTop() << ","
            << "\"xmax\":" << aBounds->GetRight() << ","
            << "\"ymax\":" << aBounds->GetBottom();
    }
    else
    {
        out << "\"xmin\":0,\"ymin\":0,\"xmax\":0,\"ymax\":0";
    }
    out << "}}\n";

    const std::string payload = out.str();
    wxFFile output( target, wxS( "ab" ) );
    if( output.IsOpened() )
        output.Write( payload.data(), payload.size() );
}

bool isPropertyEvent( wxEventType aType )
{
    return aType == wxEVT_CHECKBOX || aType == wxEVT_CHOICE || aType == wxEVT_COMBOBOX
           || aType == wxEVT_TEXT || aType == wxEVT_TEXT_ENTER || aType == wxEVT_SLIDER
           || aType == wxEVT_LISTBOX || aType == wxEVT_RADIOBUTTON
           || aType == wxEVT_TOGGLEBUTTON || aType == wxEVT_CHECKLISTBOX
           || aType == wxEVT_TREE_SEL_CHANGED || aType == wxEVT_NOTEBOOK_PAGE_CHANGED
           || aType == wxEVT_AUINOTEBOOK_PAGE_CHANGED || aType == wxEVT_GRID_CELL_CHANGED
           || aType == wxEVT_DATAVIEW_ITEM_VALUE_CHANGED || aType == wxEVT_PG_CHANGED;
}

bool isCommandEvent( wxEventType aType )
{
    return aType == wxEVT_BUTTON || aType == wxEVT_MENU || aType == wxEVT_TOOL;
}

bool isCanvasEvent( wxEventType aType )
{
    return aType == wxEVT_LEFT_UP || aType == wxEVT_RIGHT_UP || aType == wxEVT_MIDDLE_UP
           || aType == wxEVT_MOUSEWHEEL;
}

std::string propertyNameForEvent( wxEvent& aEvent, wxWindow* aWindow )
{
    if( auto* event = dynamic_cast<wxPropertyGridEvent*>( &aEvent ) )
        return toUtf8( event->GetPropertyName() );
    if( auto* event = dynamic_cast<wxGridEvent*>( &aEvent ) )
        return "row_" + std::to_string( event->GetRow() ) + ".column_"
               + std::to_string( event->GetCol() );
    if( auto* event = dynamic_cast<wxDataViewEvent*>( &aEvent ) )
        return "column_" + std::to_string( event->GetColumn() );
    const std::string name = aWindow ? toUtf8( aWindow->GetName() ) : std::string();
    return !name.empty() && name != "window" ? sanitizeIdPart( name )
                                               : "event_" + std::to_string( aEvent.GetId() );
}

bool s_catalogPublishScheduled = false;

class NATIVE_ACTION_FILTER : public wxEventFilter
{
public:
    int FilterEvent( wxEvent& aEvent ) override
    {
        const wxEventType type = aEvent.GetEventType();

        if( type == wxEVT_SHOW )
        {
            auto* showEvent = dynamic_cast<wxShowEvent*>( &aEvent );
            auto* window = dynamic_cast<wxWindow*>( aEvent.GetEventObject() );

            if( showEvent && showEvent->IsShown() && window && !s_catalogPublishScheduled )
            {
                s_catalogPublishScheduled = true;
                wxTheApp->CallAfter(
                        []
                        {
                            wxSafeYield();
                            writeVisibleActionCatalog();
                            s_catalogPublishScheduled = false;
                        } );
            }

            return Event_Skip;
        }

        if( !isPropertyEvent( type ) && !isCommandEvent( type ) && !isCanvasEvent( type ) )
            return Event_Skip;

        wxString target;
        if( !nativeActionTarget( &target ) )
            return Event_Skip;

        wxWindow* window = dynamic_cast<wxWindow*>( aEvent.GetEventObject() );
        const uint64_t interactionId = nextInteractionId();
        std::string className = "wxEvent";
        std::string label;
        std::string editor;
        wxRect      bounds;
        const wxRect* boundsPtr = nullptr;

        if( window )
        {
            className = toUtf8( window->GetClassInfo()->GetClassName() );
            label = toUtf8( window->GetLabel().empty() ? window->GetName() : window->GetLabel() );
            bounds = window->GetScreenRect();
            boundsPtr = &bounds;
            if( wxWindow* top = wxGetTopLevelParent( window ) )
                editor = toUtf8( top->GetLabel() );
        }

        if( isPropertyEvent( type ) )
        {
            const std::string propertyName = propertyNameForEvent( aEvent, window );
            appendEvent( interactionId, 0, "property_updated", "wx_event_filter", "property",
                         label, "", className, propertyName, false, true,
                         editor, boundsPtr );
        }
        else if( isCanvasEvent( type ) )
        {
            const std::string gesture = type == wxEVT_MOUSEWHEEL ? "wheel" : "pointer_release";
            appendEvent( interactionId, 0, "canvas_interaction", "wx_event_filter", "canvas",
                         className + "." + gesture, "", "", "", false, false, editor,
                         boundsPtr );
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock( s_stateMutex );
                s_pendingUiInteraction = interactionId;
            }
            wxTheApp->CallAfter(
                    [interactionId]
                    {
                        std::lock_guard<std::mutex> lock( s_stateMutex );
                        if( s_pendingUiInteraction == interactionId )
                            s_pendingUiInteraction = 0;
                    } );
            appendEvent( interactionId, 0, "ui_interaction_requested", "wx_event_filter",
                         "control", label, "", "", "", false, false, editor, boundsPtr );
        }

        writeVisibleActionCatalog();

        return Event_Skip;
    }
};

NATIVE_ACTION_FILTER s_filter;
bool                 s_installed = false;
}

namespace KICADGYM
{
std::string ClassifyNativeSemanticAction( const std::string& aCommandId,
                                          const std::string& aLabel,
                                          const std::string& aKind,
                                          const std::string& aEditor )
{
    const std::string identity = aCommandId.empty() ? ( aLabel.empty() ? aKind : aLabel )
                                                     : aCommandId;
    std::string lowered = aCommandId + " " + aLabel + " " + aKind + " " + aEditor;
    std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                    []( unsigned char value ) { return static_cast<char>( std::tolower( value ) ); } );

    std::string family = "ui";
    if( lowered.find( "gerber viewer" ) != std::string::npos
        || lowered.find( "gerbview" ) != std::string::npos )
        family = "viewer.gerber";
    else if( lowered.find( "page layout editor" ) != std::string::npos
             || lowered.find( "pl_editor" ) != std::string::npos )
        family = "drawing_sheet.editor";
    else if( lowered.find( "image converter" ) != std::string::npos
             || lowered.find( "bitmap2component" ) != std::string::npos )
        family = "image.converter";
    else if( lowered.find( "calculator" ) != std::string::npos
             || lowered.find( "pcb_calculator" ) != std::string::npos )
        family = "calculator";
    else if( lowered.find( "jobset" ) != std::string::npos
             || lowered.find( "job set" ) != std::string::npos )
        family = "project.jobs";
    else if( lowered.find( "plugin manager" ) != std::string::npos
             || lowered.find( "pcm" ) != std::string::npos )
        family = "project.plugins";
    else if( lowered.find( "gerber" ) != std::string::npos
        || lowered.find( "fabrication" ) != std::string::npos
        || lowered.find( "drill" ) != std::string::npos
        || lowered.find( "bom" ) != std::string::npos )
        family = "fabrication.export";
    else if( lowered.find( "drc" ) != std::string::npos
             || lowered.find( "design rules checker" ) != std::string::npos )
        family = "pcb.drc";
    else if( lowered.find( "erc" ) != std::string::npos
             || lowered.find( "electrical rules checker" ) != std::string::npos )
        family = "schematic.erc";
    else if( lowered.find( "assign footprint" ) != std::string::npos
             || lowered.find( "cvpcb" ) != std::string::npos )
        family = "schematic.footprint_assignment";
    else if( lowered.find( "footprint editor" ) != std::string::npos
             || lowered.find( "footprintlibrary" ) != std::string::npos )
        family = aKind == "property" ? "pcb.footprint_property" : "pcb.footprint_editor";
    else if( lowered.find( "symbol editor" ) != std::string::npos
             || lowered.find( "symbollibrary" ) != std::string::npos )
        family = aKind == "property" ? "schematic.symbol_property" : "schematic.symbol_editor";
    else if( lowered.find( "drag" ) != std::string::npos )
        family = "pcb.drag";
    else if( lowered.find( "tune" ) != std::string::npos
             || lowered.find( "meander" ) != std::string::npos
             || lowered.find( "length match" ) != std::string::npos )
        family = "pcb.tuning";
    else if( lowered.find( "zone" ) != std::string::npos
             || lowered.find( "rule area" ) != std::string::npos )
        family = "pcb.zone";
    else if( lowered.find( "via" ) != std::string::npos )
        family = "pcb.via";
    else if( lowered.find( "route" ) != std::string::npos
             || lowered.find( "router" ) != std::string::npos )
        family = "pcb.route";
    else if( lowered.find( "track" ) != std::string::npos )
        family = "pcb.track";
    else if( lowered.find( "footprint" ) != std::string::npos )
        family = aKind == "property" ? "pcb.footprint_property" : "pcb.footprint";
    else if( lowered.find( "pad" ) != std::string::npos )
        family = aKind == "property" ? "pcb.pad_property" : "pcb.pad";
    else if( lowered.find( "board setup" ) != std::string::npos
             || lowered.find( "boardsetup" ) != std::string::npos
             || lowered.find( "stackup" ) != std::string::npos )
        family = "pcb.board_setup";
    else if( lowered.find( "net inspector" ) != std::string::npos
             || lowered.find( "netclass" ) != std::string::npos
             || lowered.find( "net class" ) != std::string::npos )
        family = "pcb.net_rules";
    else if( lowered.find( "symbol" ) != std::string::npos )
        family = aKind == "property" ? "schematic.symbol_property" : "schematic.symbol";
    else if( lowered.find( "wire" ) != std::string::npos
             || lowered.find( "bus" ) != std::string::npos
             || lowered.find( "junction" ) != std::string::npos
             || lowered.find( "net label" ) != std::string::npos )
        family = "schematic.connectivity";
    else if( lowered.find( "sheet" ) != std::string::npos
             || lowered.find( "hierarch" ) != std::string::npos )
        family = "schematic.hierarchy";
    else if( lowered.find( "simulat" ) != std::string::npos
             || lowered.find( "spice" ) != std::string::npos )
        family = "schematic.simulation";
    else if( lowered.find( "3d viewer" ) != std::string::npos
             || lowered.find( "3dviewer" ) != std::string::npos )
        family = "viewer.3d";
    else if( lowered.find( "schematic" ) != std::string::npos
             || lowered.find( "eeschema" ) != std::string::npos )
        family = aKind == "canvas" ? "schematic.canvas" : "schematic.ui";
    else if( lowered.find( "pcb" ) != std::string::npos
             || lowered.find( "pcbnew" ) != std::string::npos )
        family = aKind == "canvas" ? "pcb.canvas" : "pcb.ui";
    else if( lowered.find( "project" ) != std::string::npos )
        family = "project";

    return "kicad." + family + "." + sanitizeIdPart( identity );
}

void InstallNativeActionCapture()
{
    wxString actionTarget;
    if( !nativeActionTarget( &actionTarget ) )
        return;

    if( !s_installed )
    {
        wxEvtHandler::AddFilter( &s_filter );
        s_installed = true;
    }
}

void UninstallNativeActionCapture()
{
    if( s_installed )
    {
        wxEvtHandler::RemoveFilter( &s_filter );
        s_installed = false;
    }
}

uint64_t BeginNativeCommand( const std::string& aCommandId )
{
    std::lock_guard<std::mutex> lock( s_stateMutex );
    const uint64_t interactionId = s_pendingUiInteraction != 0
                                           ? s_pendingUiInteraction
                                           : nextInteractionId();
    s_pendingUiInteraction = 0;
    s_pendingTransactionParent = interactionId;
    s_commandStack.push_back( interactionId );
    return interactionId;
}

void FinishNativeCommand( uint64_t aInteractionId, const std::string& aCommandId,
                          const wxString& aLabel, bool aCompleted, bool aDeferred )
{
    appendEvent( aInteractionId, 0, aDeferred ? "command_dispatched" : "command_finished",
                 "tool_manager", "command", toUtf8( aLabel ), aCommandId, "", "",
                 aCompleted, false, "", nullptr );
    writeVisibleActionCatalog();

    std::lock_guard<std::mutex> lock( s_stateMutex );
    if( !s_commandStack.empty() && s_commandStack.back() == aInteractionId )
        s_commandStack.pop_back();
}

void RecordNativeTransaction( const char* aEditor, const wxString& aLabel )
{
    uint64_t parentInteractionId = 0;
    {
        std::lock_guard<std::mutex> lock( s_stateMutex );
        if( !s_commandStack.empty() )
            parentInteractionId = s_commandStack.back();
        else
            parentInteractionId = s_pendingTransactionParent;
        s_pendingTransactionParent = 0;
    }
    appendEvent( nextInteractionId(), parentInteractionId, "transaction_committed", "commit",
                 "transaction", toUtf8( aLabel ), "", "", "", false, false,
                 aEditor ? aEditor : "", nullptr );
    writeVisibleActionCatalog();
}
}
