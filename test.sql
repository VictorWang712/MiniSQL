-- create database
create database db0;
create database db1;
create database db2;
show databases;

-- create table
use db0;
create table account(
  id int,
  name char(16),
  balance float,
  primary key(id)
);
show tables;
show indexes;

-- insert data
execfile "../sql_gen/account00.txt";
execfile "../sql_gen/account01.txt";
execfile "../sql_gen/account02.txt";
execfile "../sql_gen/account03.txt";
execfile "../sql_gen/account04.txt";
execfile "../sql_gen/account05.txt";
execfile "../sql_gen/account06.txt";
execfile "../sql_gen/account07.txt";
execfile "../sql_gen/account08.txt";
execfile "../sql_gen/account09.txt";
use db0;
select * from account;

-- point query
use db0;
select * from account where id = 12556789;
select * from account where balance = 177.50;
select * from account where name = "name56789"; -- record execution time t_1
select * from account where id <> 12556789;
select * from account where balance <> 177.50;
select * from account where name <> "name56789";

-- multi-condition queries and projection operations
use db0;
select id, name from account where balance >= 100 and balance < 200;
select name, balance from account where balance > 500 and id <= 12510000;
select * from account where id < 12515000 and name > "name14500";
select * from account where id < 12500200 and name < "name00100"; -- record execution time t_5

-- unique constraint
use db0;
create table account_u(
  id int,
  name char(16) unique,
  balance float,
  primary key(id)
);
insert into account_u values(1, "alice", 100.0);
insert into account_u values(1, "bob", 200.0);
insert into account_u values(2, "alice", 300.0);

-- index
use db0;
create index idx01 on account(name);
show indexes;
select * from account where name = "name56789"; -- record execution time t_2
select * from account where name = "name45678"; -- record execution time t_3
select * from account where id < 12500200 and name < "name00100"; -- record execution time t_6
delete from account where name = "name45678";
insert into account values(12545678, "name45678", 123.45);
drop index idx01;
show indexes;
select * from account where name = "name45678"; -- record execution time t_4

-- update
use db0;
update account set id = 22556789, balance = 888.88 where name = "name56789";
select * from account where name = "name56789";

-- delete
use db0;
delete from account where name = "name45678";
select * from account where name = "name45678";
delete from account;
select * from account;
drop table account;
show tables;
