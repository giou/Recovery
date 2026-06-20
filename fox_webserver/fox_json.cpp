/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
*/

#include "fox_json.hpp"

#include <memory>

namespace {

Json::Value parse_raw(const std::string& json)
{
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;

	Json::Value value;
	std::string errors;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (reader->parse(json.data(), json.data() + json.size(), &value, &errors))
		return value;

	return Json::Value();
}

std::string write_json(const Json::Value& value)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	builder["commentStyle"] = "None";
	return Json::writeString(builder, value);
}

} // namespace

Fox_Json_Object::Fox_Json_Object()
	: value_(Json::objectValue)
{
}

Fox_Json_Object& Fox_Json_Object::AddString(const std::string& key, const std::string& value)
{
	value_[key] = value;
	return *this;
}

Fox_Json_Object& Fox_Json_Object::AddInt(const std::string& key, long long value)
{
	value_[key] = Json::Int64(value);
	return *this;
}

Fox_Json_Object& Fox_Json_Object::AddSize(const std::string& key, size_t value)
{
	value_[key] = Json::UInt64(value);
	return *this;
}

Fox_Json_Object& Fox_Json_Object::AddBool(const std::string& key, bool value)
{
	value_[key] = value;
	return *this;
}

Fox_Json_Object& Fox_Json_Object::AddRaw(const std::string& key, const std::string& json)
{
	value_[key] = parse_raw(json);
	return *this;
}

std::string Fox_Json_Object::String() const
{
	return write_json(value_);
}

Fox_Json_Array::Fox_Json_Array()
	: value_(Json::arrayValue)
{
}

Fox_Json_Array& Fox_Json_Array::AddRaw(const std::string& json)
{
	value_.append(parse_raw(json));
	return *this;
}

Fox_Json_Array& Fox_Json_Array::AddString(const std::string& value)
{
	value_.append(value);
	return *this;
}

std::string Fox_Json_Array::String() const
{
	return write_json(value_);
}
