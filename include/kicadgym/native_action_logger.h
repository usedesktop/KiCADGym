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

#ifndef KICADGYM_NATIVE_ACTION_LOGGER_H
#define KICADGYM_NATIVE_ACTION_LOGGER_H

#include <cstdint>
#include <string>

#include <kicommon.h>
#include <wx/string.h>

namespace KICADGYM
{
KICOMMON_API void InstallNativeActionCapture();
KICOMMON_API void UninstallNativeActionCapture();

KICOMMON_API std::string ClassifyNativeSemanticAction( const std::string& aCommandId,
                                                       const std::string& aLabel,
                                                       const std::string& aKind,
                                                       const std::string& aEditor );

KICOMMON_API uint64_t BeginNativeCommand( const std::string& aCommandId );
KICOMMON_API void FinishNativeCommand( uint64_t aInteractionId,
                                       const std::string& aCommandId,
                                       const wxString& aLabel,
                                       bool aCompleted,
                                       bool aDeferred );
KICOMMON_API void RecordNativeTransaction( const char* aEditor, const wxString& aLabel );
}

#endif
