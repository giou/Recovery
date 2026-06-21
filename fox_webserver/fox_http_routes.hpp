/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#ifndef _FOX_HTTP_ROUTES_HPP
#define _FOX_HTTP_ROUTES_HPP

#include "fox_io.hpp"

namespace Fox_Http {

// Drive a single HTTP(S) connection to completion. conn may be plaintext or
// TLS; it owns its fd/SSL and is closed by this call.
void HandleConnection(Fox_Io::Conn conn);

} // namespace Fox_Http

#endif // _FOX_HTTP_ROUTES_HPP
