grammar Tisa;

program: 'state' '{' bank+ '}' instruction+ EOF;
bank: ID ':' ('Tile' '<' element ',' UINT ',' UINT '>' | element) '[' UINT ']';
element: 'i8' | 'i32';
instruction
    : 'instruction' ID '(' (operand (',' operand)*)? ')' '{'
      'requires' comparison ('&&' comparison)*
      'reads' ('none' | readList=locations)
      'writes' writeList=locations
      equation+
      '}'
    ;
operand: ID ':' 'u32';
comparison: indexExpr op=('<' | '<=') indexExpr;
locations: location (',' location)*;
location: ID '[' indexExpr (':' indexExpr)? ']';
equation: 'ensures' ID '\'' '[' indexExpr (':' indexExpr)? ']' '=' expression;
indexExpr: indexTerm ('+' indexTerm)*;
indexTerm: UINT | ID;
expression: primary ('+' primary)*;
primary
    : location
    | tileLiteral
    | op='matmul_int' '(' expression ',' expression ')'
    | op='wrap_i32' '(' expression ')'
    ;
tileLiteral: '[' row (',' row)* ']';
row: '[' integer (',' integer)* ']';
integer: '-'? UINT;

ID: [a-zA-Z_] [a-zA-Z_0-9]*;
UINT: [0-9]+;
WS: [ \t\r\n]+ -> skip;
