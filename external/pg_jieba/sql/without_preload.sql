CREATE EXTENSION pg_jieba;

select * from to_tsvector('jiebacfg', '小明硕士毕业于中国科学院计算所，后在日本京都大学深造');

select * from to_tsvector('jiebacfg', '李小福是创新办主任也是云计算方面的专家');

insert into jieba_user_dict values('阿里云');

insert into jieba_user_dict values('研发工程师');

insert into jieba_user_dict values('阿里',1);

select jieba_load_user_dict(0);

select * from to_tsvector('jiebacfg', 'zth是阿里云的一个研发工程师');

select jieba_load_user_dict(1);

select * from to_tsvector('jiebacfg', 'zth是阿里云的一个研发工程师');

select * from to_tsvector('jiebacfg_pos', 'zth是阿里云的一个研发工程师');

select * from to_tsvector('jiebacfg_pos', 'zth是alicloud💻的一个👨研发工程师');