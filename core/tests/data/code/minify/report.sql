-- report
select u.id, u.name, count(o.id) as orders /* inline */
from users u left join orders o on o.user_id = u.id and o.status <> 'it''s -- not a comment'
where u.created_at > '2024-01-01' and (u.flag = 1 or u.flag is null) and u.id in (select id from vip)
group by u.id, u.name having count(o.id) > 2 order by orders desc limit 10;
