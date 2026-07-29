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

#include <boost/test/unit_test.hpp>

#include <array>
#include <string>

#include <kicadgym/native_action_logger.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>

BOOST_AUTO_TEST_SUITE( KiCadGymNativeActionCatalog )

struct SEMANTIC_CASE
{
    const char* command;
    const char* label;
    const char* kind;
    const char* editor;
    const char* prefix;
};

BOOST_AUTO_TEST_CASE( CoversGoldenEditorAndCanvasSemantics )
{
    const std::array cases{
        SEMANTIC_CASE{ "pcbnew.InteractiveRouter.RouteSingleTrack", "Route Track", "command", "PCB Editor", "kicad.pcb.route." },
        SEMANTIC_CASE{ "pcbnew.InteractiveEdit.drag", "Drag", "command", "PCB Editor", "kicad.pcb.drag." },
        SEMANTIC_CASE{ "pcbnew.InteractiveEdit.dragKeepSlope", "Drag Keep Slope", "command", "PCB Editor", "kicad.pcb.drag." },
        SEMANTIC_CASE{ "pcbnew.InteractiveTune.tuneSingleTrack", "Tune Track Length", "command", "PCB Editor", "kicad.pcb.tuning." },
        SEMANTIC_CASE{ "pcbnew.InteractiveTune.tuneDiffPair", "Tune Differential Pair", "command", "PCB Editor", "kicad.pcb.tuning." },
        SEMANTIC_CASE{ "pcbnew.InteractiveDrawing.drawZone", "Add Filled Zone", "command", "PCB Editor", "kicad.pcb.zone." },
        SEMANTIC_CASE{ "pcbnew.InteractiveDrawing.drawRuleArea", "Add Rule Area", "command", "PCB Editor", "kicad.pcb.zone." },
        SEMANTIC_CASE{ "pcbnew.InteractiveRouter.placeVia", "Place Via", "command", "PCB Editor", "kicad.pcb.via." },
        SEMANTIC_CASE{ "pcbnew.InteractiveEdit.editTracksAndVias", "Edit Tracks", "command", "PCB Editor", "kicad.pcb.via." },
        SEMANTIC_CASE{ "pcbnew.InteractivePlace.placeFootprint", "Place Footprint", "command", "PCB Editor", "kicad.pcb.footprint." },
        SEMANTIC_CASE{ "", "Reference", "property", "Footprint Properties", "kicad.pcb.footprint_property." },
        SEMANTIC_CASE{ "", "Pad Number", "property", "Pad Properties", "kicad.pcb.pad_property." },
        SEMANTIC_CASE{ "pcbnew.InteractiveDrawing.placePad", "Place Pad", "command", "Footprint Editor", "kicad.pcb.footprint_editor." },
        SEMANTIC_CASE{ "pcbnew.BoardSetup", "Board Setup Stackup", "command", "PCB Editor", "kicad.pcb.board_setup." },
        SEMANTIC_CASE{ "pcbnew.NetInspector", "Net Inspector", "command", "PCB Editor", "kicad.pcb.net_rules." },
        SEMANTIC_CASE{ "pcbnew.InspectionTool.runDRC", "Design Rules Checker", "command", "PCB Editor", "kicad.pcb.drc." },
        SEMANTIC_CASE{ "eeschema.InteractiveDrawing.placeSymbol", "Place Symbol", "command", "Schematic Editor", "kicad.schematic.symbol." },
        SEMANTIC_CASE{ "", "Value", "property", "Symbol Properties", "kicad.schematic.symbol_property." },
        SEMANTIC_CASE{ "eeschema.SymbolLibraryEditor", "Symbol Editor", "command", "Symbol Editor", "kicad.schematic.symbol_editor." },
        SEMANTIC_CASE{ "eeschema.InteractiveDrawing.drawWire", "Draw Wire", "command", "Schematic Editor", "kicad.schematic.connectivity." },
        SEMANTIC_CASE{ "eeschema.InteractiveDrawing.drawBus", "Draw Bus", "command", "Schematic Editor", "kicad.schematic.connectivity." },
        SEMANTIC_CASE{ "eeschema.InteractiveDrawing.placeHierarchicalSheet", "Place Sheet", "command", "Schematic Editor", "kicad.schematic.hierarchy." },
        SEMANTIC_CASE{ "eeschema.InspectionTool.runERC", "Electrical Rules Checker", "command", "Schematic Editor", "kicad.schematic.erc." },
        SEMANTIC_CASE{ "eeschema.Simulator.show", "Simulator", "command", "Schematic Editor", "kicad.schematic.simulation." },
        SEMANTIC_CASE{ "cvpcb.AssignFootprints", "Assign Footprints", "command", "Assign Footprints", "kicad.schematic.footprint_assignment." },
        SEMANTIC_CASE{ "pcbnew.ExportGerbers", "Plot Gerbers", "command", "PCB Editor", "kicad.fabrication.export." },
        SEMANTIC_CASE{ "", "GAL Canvas.pointer_release", "canvas", "PCB Editor", "kicad.pcb.canvas." },
        SEMANTIC_CASE{ "", "SCH Canvas.wheel", "canvas", "Schematic Editor", "kicad.schematic.canvas." },
        SEMANTIC_CASE{ "common.Control.show3DViewer", "3D Viewer", "command", "3D Viewer", "kicad.viewer.3d." },
        SEMANTIC_CASE{ "gerbview.Control.open", "Open Gerber", "command", "Gerber Viewer", "kicad.viewer.gerber." },
        SEMANTIC_CASE{ "pl_editor.Control.placeText", "Place Text", "command", "Page Layout Editor", "kicad.drawing_sheet.editor." },
        SEMANTIC_CASE{ "bitmap2component.Convert", "Image Converter", "command", "Image Converter", "kicad.image.converter." },
        SEMANTIC_CASE{ "pcb_calculator.Open", "Calculator", "command", "Calculator", "kicad.calculator." },
        SEMANTIC_CASE{ "kicad.Jobset.Run", "Run Job Set", "command", "KiCad", "kicad.project.jobs." },
        SEMANTIC_CASE{ "kicad.PCM.Open", "Plugin Manager", "command", "KiCad", "kicad.project.plugins." },
    };

    for( const SEMANTIC_CASE& semanticCase : cases )
    {
        const std::string result = KICADGYM::ClassifyNativeSemanticAction(
                semanticCase.command, semanticCase.label, semanticCase.kind, semanticCase.editor );
        BOOST_TEST_CONTEXT( semanticCase.command << " / " << semanticCase.label )
        {
            BOOST_CHECK_EQUAL( result.rfind( semanticCase.prefix, 0 ), 0 );
        }
    }
}

BOOST_AUTO_TEST_CASE( EmitsStableCommandAndCommitContract )
{
    const wxString controlPath = wxFileName::CreateTempFileName( wxS( "kicadgym-control" ) );
    const wxString actionPath = controlPath + wxS( ".jsonl" );
    {
        wxFile control( controlPath, wxFile::write );
        BOOST_REQUIRE( control.IsOpened() );
        const std::string target = actionPath.ToStdString();
        BOOST_REQUIRE_EQUAL( control.Write( target.data(), target.size() ), target.size() );
    }

    wxSetEnv( wxS( "RL_ENV_NATIVE_ACTION_CONTROL_FILE" ), controlPath );
    wxSetEnv( wxS( "RL_ENV_SESSION_ID" ), wxS( "kicad-test-session" ) );
    wxSetEnv( wxS( "RL_ENV_KIND" ), wxS( "kicadgym" ) );

    const uint64_t interactionId = KICADGYM::BeginNativeCommand(
            "pcbnew.InteractiveRouter.RouteSingleTrack" );
    KICADGYM::FinishNativeCommand( interactionId,
                                  "pcbnew.InteractiveRouter.RouteSingleTrack",
                                  wxS( "Route Track" ), true, true );
    KICADGYM::RecordNativeTransaction( "PCB Editor", wxS( "Route Track" ) );

    wxFile actionLog( actionPath, wxFile::read );
    BOOST_REQUIRE( actionLog.IsOpened() );
    wxString payload;
    BOOST_REQUIRE( actionLog.ReadAll( &payload ) );
    const std::string jsonl = payload.ToStdString();
    BOOST_CHECK_NE( jsonl.find( "\"schema\":\"rl_env.native_action.v1\"" ), std::string::npos );
    BOOST_CHECK_NE( jsonl.find( "\"catalog_id\":\"kicad.command.pcbnew_interactiverouter_routesingletrack\"" ), std::string::npos );
    BOOST_CHECK_NE( jsonl.find( "\"semantic_action_id\":\"kicad.pcb.route." ), std::string::npos );
    BOOST_CHECK_NE( jsonl.find( "\"verifier_bindings\":[" ), std::string::npos );
    BOOST_CHECK_NE( jsonl.find( "\"phase\":\"transaction_committed\"" ), std::string::npos );
    BOOST_CHECK_NE( jsonl.find( "\"parent_interaction_id\":" + std::to_string( interactionId ) ),
                    std::string::npos );
    actionLog.Close();

    wxUnsetEnv( wxS( "RL_ENV_NATIVE_ACTION_CONTROL_FILE" ) );
    wxUnsetEnv( wxS( "RL_ENV_SESSION_ID" ) );
    wxUnsetEnv( wxS( "RL_ENV_KIND" ) );
    wxRemoveFile( controlPath );
    wxRemoveFile( actionPath );
    const wxString statePath = wxFileName( actionPath ).GetPathWithSep() + wxS( "ui-state.json" );
    if( wxFileExists( statePath ) )
        wxRemoveFile( statePath );
}

BOOST_AUTO_TEST_SUITE_END()
