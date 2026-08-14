# SQLite amalgamation

RenderMesh statically embeds SQLite 3.53.4 from the official amalgamation:
https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip

SQLite is in the public domain. The downloaded archive SHA-256 was
`1E71DDF93849C6A6ECF58B827C0692073D2DD7EE40196158068F7B29F422E87D`.

The build disables loadable extensions and double-quoted string literals.
