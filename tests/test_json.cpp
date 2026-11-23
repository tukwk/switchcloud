#include "json.h"
#include <cstdio>
#include <cassert>
int main(){
  const char* s = R"({"collection":[{"id":123456,"title":"Тест \"track\"","user":{"username":"Юзер"},"duration":215000,"streamable":true,"artwork_url":"https://x/art.jpg","media":{"transcodings":[{"url":"https://api/tr/1","format":{"protocol":"progressive","mime_type":"audio/mpeg"}}]}},{"id":2,"title":"B","user":{"username":"U2"},"duration":1000}],"next_href":null})";
  Json j; std::string err;
  if(!Json::parse(s,j,&err)){ printf("parse fail: %s\n",err.c_str()); return 1; }
  const Json* c=j.get("collection");
  assert(c && c->size()==2);
  assert(c->at(0)->get("id")->integer()==123456);
  assert(c->at(0)->get("title")->string()=="Тест \"track\"");
  assert(c->at(0)->get("user")->get("username")->string()=="Юзер");
  assert(c->at(0)->get("duration")->integer()==215000);
  const Json* t=c->at(0)->get("media")->get("transcodings")->at(0);
  assert(t->get("format")->get("protocol")->string()=="progressive");
  assert(c->at(0)->get("missing")==nullptr);
  // unicode escape + surrogate pair
  Json u; assert(Json::parse("\"\\u041f\\u0440\\u0438\\u0432\\u0435\\u0442 \\ud83c\\udfa7\"",u));
  printf("unicode: %s\n", u.string().c_str());
  printf("OK\n");
}
