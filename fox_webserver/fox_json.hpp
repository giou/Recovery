/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#ifndef _FOX_JSON_HPP
#define _FOX_JSON_HPP

#include <cstddef>
#include <string>

#include <json/json.h>

class Fox_Json_Object
{
public:
	Fox_Json_Object();

	Fox_Json_Object& AddString(const std::string& key, const std::string& value);
	Fox_Json_Object& AddInt(const std::string& key, long long value);
	Fox_Json_Object& AddSize(const std::string& key, size_t value);
	Fox_Json_Object& AddBool(const std::string& key, bool value);
	Fox_Json_Object& AddRaw(const std::string& key, const std::string& json);

	std::string String() const;

private:
	Json::Value value_;
};

class Fox_Json_Array
{
public:
	Fox_Json_Array();

	Fox_Json_Array& AddRaw(const std::string& json);
	Fox_Json_Array& AddString(const std::string& value);

	std::string String() const;

private:
	Json::Value value_;
};

#endif // _FOX_JSON_HPP
